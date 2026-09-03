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

#include "layout_engine.h"
#include "text_renderer.h"
#include <stdint.h>
#include <string.h>

#ifndef ERUI_MAX_NODES
#define ERUI_MAX_NODES 512
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Per-child scratch record used during a single container's layout pass.
 *
 * Populated in Pass 1 for each in-flow child and consumed by Passes 2–6.
 * The array is reused for each container level; it is safe across recursion
 * because results are written back to the scene graph before descending.
 */
typedef struct
{
    uint16_t tag;
    int16_t main;  /**< Resolved main-axis size. */
    int16_t cross; /**< Resolved cross-axis size. */
    int16_t margin_main_start;
    int16_t margin_main_end;
    int16_t margin_cross_start;
    int16_t margin_cross_end;
    int16_t main_pos;  /**< Offset within parent content along the main axis. */
    int16_t cross_pos; /**< Offset within parent content along the cross-axis. */
    int16_t flex_grow;
    int16_t flex_shrink;
    int16_t main_min; /**< Main-axis min constraint (ER_LAYOUT_AUTO = none). */
    int16_t main_max; /**< Main-axis max constraint (ER_LAYOUT_AUTO = none). */
    uint8_t line;     /**< Wrap-line index. */
    uint8_t align;    /**< Resolved align (auto → parent align_items). */
    uint8_t frozen;   /**< Pass 3: 1 once the item's flexed main size is final. */
} FlexChild;

/**
 * @brief Per-pass memoisation slot for measure_content(), keyed by node tag.
 *
 * measure_content() is pure intrinsic sizing — for a given tag it depends only on that
 * node's own subtree, which nothing mutates during a layout pass — so its result is a
 * constant for the whole er_layout_compute() call. Without this cache, compute_layout()
 * re-measures every child once per Pass 1 it participates in *and* recurses into that
 * same child, so a leaf at ancestor depth d gets remeasured d times. The `gen` tag makes
 * the cache self-invalidating across passes without a memset: a slot is only valid when
 * its gen matches the current pass's s_measure_gen.
 */
typedef struct
{
    int16_t w, h;
    uint16_t gen;
} MeasureCacheEntry;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static FlexChild s_scratch[ERUI_MAX_NODES];
static int16_t s_line_cross[ERUI_MAX_NODES];
static MeasureCacheEntry s_measure_cache[ERUI_MAX_NODES];
static uint16_t s_measure_gen; /**< Current layout pass id; 0 means "no pass has run yet". */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Resolves a per-edge value, falling back to shorthand then 0.
 *
 * @param[in] edge       Per-edge value (ER_LAYOUT_AUTO = not set).
 * @param[in] shorthand  Shorthand value (ER_LAYOUT_AUTO = not set).
 *
 * @return The resolved value, or 0 if neither is set.
 */
static int16_t edge_or(const int16_t edge, const int16_t shorthand)
{
    if (edge != ER_LAYOUT_AUTO)
        return edge;
    if (shorthand != ER_LAYOUT_AUTO)
        return shorthand;
    return 0;
}

/**
 * @brief Clamps v to [mn, mx], treating ER_LAYOUT_AUTO bounds as unconstrained.
 *
 * @param[in] v   Value to clamp.
 * @param[in] mn  Minimum bound (ER_LAYOUT_AUTO = no minimum).
 * @param[in] mx  Maximum bound (ER_LAYOUT_AUTO = no maximum).
 *
 * @return The clamped value, never negative.
 */
static int16_t clamp_size(int16_t v, const int16_t mn, const int16_t mx)
{
    if (mn != ER_LAYOUT_AUTO && v < mn)
        v = mn;
    if (mx != ER_LAYOUT_AUTO && v > mx)
        v = mx;
    if (v < 0)
        v = 0;
    return v;
}

/**
 * @brief Divides and rounds a half up, matching Yoga's pixel grid.
 *
 * Layout positions round the way Yoga does: a coordinate on a half pixel goes up. Plain C division
 * truncates instead, which lands a pixel short.
 *
 * Round a position ONCE, after everything that shifts it — including the mirror a reversed axis
 * applies. `num` may carry the whole-pixel part of the position or only the fraction; Pass 5 leaves
 * it out just to keep the operands small.
 *
 * @param[in] num  Numerator. May be negative: an item wider than the box it is centred in overhangs.
 * @param[in] den  Denominator. Must be > 0.
 *
 * @return floor(num / den + 1/2).
 */
static int32_t div_round(const int32_t num, const int32_t den)
{
    int32_t q = num / den;
    int32_t r = num % den; /* Same sign as num (C99 6.5.5p6). */

    /* Renormalise to a floor quotient with a remainder in [0, den), so the half-up test below reads
     * the same for negative numerators as for positive ones. */
    if (r < 0)
    {
        q--;
        r += den;
    }
    if (2 * r >= den)
        q++;
    return q;
}

/**
 * @brief Returns true when d is a row-based flex direction.
 *
 * @param[in] d  ERFlexDirection value.
 *
 * @return true for ER_FLEX_ROW or ER_FLEX_ROW_REVERSE, false otherwise.
 */
static bool is_row_dir(const uint8_t d)
{
    return d == ER_FLEX_ROW || d == ER_FLEX_ROW_REVERSE;
}

/**
 * @brief Returns true when d is a reversed flex direction.
 *
 * @param[in] d  ERFlexDirection value.
 *
 * @return true for ER_FLEX_ROW_REVERSE or ER_FLEX_COL_REVERSE, false otherwise.
 */
static bool is_reverse_dir(const uint8_t d)
{
    return d == ER_FLEX_ROW_REVERSE || d == ER_FLEX_COL_REVERSE;
}

/**
 * @brief Main-axis offset of a SOLE flex item under justifyContent, for a static position.
 *
 * CSS defines the static position of an absolutely positioned flex child as the spot it would take
 * "as if it were the sole flex item", and Yoga agrees — in-flow siblings never move it. That makes
 * every distribution mode collapse: with nothing to distribute between, space-between degenerates to
 * flex-start, and space-around / space-evenly both put the whole free space either side of the item,
 * i.e. centre it. These are Pass 5's own formulas evaluated at count == 1.
 *
 * Returned in HALF pixels: solo_axis_pos() may still mirror the offset for a reversed axis, and
 * mirroring is only correct before rounding.
 *
 * @param[in] justify    ERFlexJustify value from the parent.
 * @param[in] remaining  Free main-axis space: available - item - its main margins. May be NEGATIVE.
 *
 * @return TWICE the offset from the content-box main start, before the item's own leading margin.
 */
static int32_t solo_main_offset2(const uint8_t justify, const int16_t remaining)
{
    switch (justify)
    {
        case ER_JUSTIFY_CENTER:
        case ER_JUSTIFY_SPACE_AROUND:
        case ER_JUSTIFY_SPACE_EVENLY:
            return remaining;
        case ER_JUSTIFY_FLEX_END:
            return 2 * (int32_t)remaining;
        default: /* ER_JUSTIFY_FLEX_START, ER_JUSTIFY_SPACE_BETWEEN */
            return 0;
    }
}

/**
 * @brief Cross-axis offset of a SOLE flex item under a resolved alignSelf, for a static position.
 *
 * STRETCH lands with FLEX_START on purpose: an absolute is never stretched to its containing block,
 * so `alignItems: 'stretch'` (the default) leaves an auto-sized absolute at its content size in the
 * cross-start corner. Only the placement modes move it.
 *
 * In HALF pixels, like solo_main_offset2().
 *
 * @param[in] align      ERFlexAlign value, already resolved through align_self / align_items.
 * @param[in] remaining  Free cross-axis space: available - item - its cross margins. May be NEGATIVE.
 *
 * @return TWICE the offset from the content-box cross start, before the item's own leading margin.
 */
static int32_t solo_cross_offset2(const uint8_t align, const int16_t remaining)
{
    switch (align)
    {
        case ER_ALIGN_CENTER:
            return remaining;
        case ER_ALIGN_FLEX_END:
            return 2 * (int32_t)remaining;
        default: /* ER_ALIGN_FLEX_START, ER_ALIGN_STRETCH */
            return 0;
    }
}

/**
 * @brief Turns a solo_*_offset() into a screen coordinate on one axis.
 *
 * A reversed axis (flexDirection: *-reverse on the main axis, flexWrap: wrap-reverse on the cross)
 * measures from the far edge, and the margin that leads is the one on THAT side — so the trailing
 * margin becomes the gap the item sits behind. `remaining` is deliberately not clamped at 0 by the
 * caller: an item larger than the space it is centred in overhangs both edges symmetrically, which
 * is what Yoga does and what makes a too-big centred overlay stay centred instead of sticking.
 *
 * @param[in] start    Screen coordinate of the content box on this axis.
 * @param[in] avail    Content-box extent on this axis.
 * @param[in] size     The item's resolved size on this axis.
 * @param[in] m_lead   Margin on the axis-start side.
 * @param[in] m_trail  Margin on the axis-end side.
 * @param[in] offset2  TWICE the offset, from solo_main_offset2() / solo_cross_offset2().
 * @param[in] reversed Whether this axis runs from the far edge.
 *
 * @return The item's screen origin on this axis.
 */
static int16_t solo_axis_pos(const int16_t start,
                             const int16_t avail,
                             const int16_t size,
                             const int16_t m_lead,
                             const int16_t m_trail,
                             const int32_t offset2,
                             const bool reversed)
{
    /* Stay in half pixels until the mirror is done, then round once. */
    const int32_t pos2 = reversed ? (2 * (int32_t)(avail - m_trail - size) - offset2) : (offset2 + 2 * (int32_t)m_lead);
    return (int16_t)(start + div_round(pos2, 2));
}

static void measure_content(const uint16_t tag, int16_t* out_w, int16_t* out_h);

/**
 * @brief Measures a child's intrinsic content size at most once, on first use.
 *
 * Most Pass 1 children resolve both axes from explicit size / percentage / flex_basis
 * and never touch the intrinsic size at all — measuring them anyway would recurse their
 * whole subtree for nothing. Callers thread `measured`/`iw`/`ih` through the main-axis and
 * cross-axis resolution so the (potentially expensive) measure_content() call happens on
 * demand and is shared between the two axes when both need it.
 *
 * @param[in]      tag       Tag of the child to measure.
 * @param[in,out]  measured  Set to true once this call has measured; skips re-measuring.
 * @param[out]     iw        Receives the intrinsic width (only written on the first call).
 * @param[out]     ih        Receives the intrinsic height (only written on the first call).
 */
static void measure_lazy(const uint16_t tag, bool* measured, int16_t* iw, int16_t* ih)
{
    if (*measured)
        return;
    measure_content(tag, iw, ih);
    *measured = true;
}

/**
 * @brief Computes a node's intrinsic content size, independent of the space its parent allocates.
 *
 * Leaf Text nodes measure their glyph run. Container nodes recurse into in-flow children and
 * combine per the container's flex_direction: the main axis sums child outer sizes (plus gaps),
 * the cross axis takes the largest child outer size; the node's own padding is then added. An
 * explicit width/height short-circuits measurement on that axis.
 *
 * This gives Pass 1 a non-zero hypothetical size for auto-sized containers, so a container with
 * no explicit size on an axis grows to fit its children instead of collapsing to zero — e.g. a
 * `flexDirection: row` View with no height reports the height of its tallest child rather than 0.
 * Flex grow/shrink is intentionally ignored here (this is max-content sizing); the parent applies
 * flex distribution later against the available space.
 *
 * The walk is self-contained — it never reads or writes s_scratch / s_line_cross — so it is safe
 * to call from within compute_layout's Pass 1 without disturbing the in-progress layout state.
 *
 * @param[in]  tag    Tag of the node to measure.
 * @param[out] out_w  Receives the intrinsic width in pixels.
 * @param[out] out_h  Receives the intrinsic height in pixels.
 */
static void measure_content(const uint16_t tag, int16_t* out_w, int16_t* out_h)
{
    /* Per-pass memoisation: measure_content(tag) is a pure function of that node's own
     * subtree for the duration of one er_layout_compute() call, so a cache hit here is
     * exactly what turns the O(depth) remeasurement into O(1) per node per pass. */
    const bool cacheable = tag < (uint16_t)ERUI_MAX_NODES;
    if (cacheable && s_measure_cache[tag].gen == s_measure_gen)
    {
        *out_w = s_measure_cache[tag].w;
        *out_h = s_measure_cache[tag].h;
        return;
    }

    ERNode* n = er_get_node(tag);
    if (!n)
    {
        *out_w = 0;
        *out_h = 0;
        return;
    }

    const ERLayoutSpec* L = &n->layout;
    const int16_t exp_w = L->width;
    const int16_t exp_h = L->height;

    /* Text leaf: measure the glyph run, then add the node's own padding — the glyphs are this node's
     * content, so an auto axis has to grow around them exactly as a container grows around a child
     * (the compositor insets the render clip by the same padding, so the two agree). An EXPLICIT
     * width/height already includes its padding, as everywhere else here. Both axes explicit means
     * the measurement result is never consulted below, so skip it. */
    if (n->type == ER_NODE_TEXT)
    {
        int16_t tw, th;
        if (exp_w != ER_LAYOUT_AUTO && exp_h != ER_LAYOUT_AUTO)
        {
            tw = exp_w;
            th = exp_h;
        }
        else
        {
            const ERPadding tp = er_layout_padding(L);
            int measured_w = 0, measured_h = 0;
            if (n->props.text.span_count > 0U)
            {
                /* Styled spans render with per-run weight/spacing — measure them the same way so a
                   bold/styled run does not overflow an auto-sized node and clip its trailing glyphs. */
                er_text_measure_spans(n->props.text.spans,
                                      n->props.text.span_count,
                                      n->props.text.font_size,
                                      n->props.text.font_family,
                                      n->props.text.letter_spacing,
                                      n->props.text.font_weight,
                                      &measured_w,
                                      &measured_h);
            }
            else
            {
                er_text_measure(n->props.text.text,
                                n->props.text.font_size,
                                n->props.text.font_family,
                                n->props.text.letter_spacing,
                                n->props.text.font_weight,
                                &measured_w,
                                &measured_h);
            }
            const int16_t line_h = (n->props.text.line_height > 0) ? n->props.text.line_height : (int16_t)measured_h;
            const int lines = (n->props.text.number_of_lines > 1) ? (int)n->props.text.number_of_lines : 1;
            tw = (exp_w != ER_LAYOUT_AUTO) ? exp_w : (int16_t)(measured_w + tp.left + tp.right);
            th = (exp_h != ER_LAYOUT_AUTO) ? exp_h : (int16_t)(line_h * lines + tp.top + tp.bottom);
        }
        *out_w = clamp_size(tw, L->min_width, L->max_width);
        *out_h = clamp_size(th, L->min_height, L->max_height);
        if (cacheable)
        {
            s_measure_cache[tag].w = *out_w;
            s_measure_cache[tag].h = *out_h;
            s_measure_cache[tag].gen = s_measure_gen;
        }
        return;
    }

    /* Container / leaf View: derive content size from in-flow children (only when an axis is auto). */
    int16_t content_w = 0, content_h = 0;
    if (n->first_child_tag != ER_INVALID_TAG && (exp_w == ER_LAYOUT_AUTO || exp_h == ER_LAYOUT_AUTO))
    {
        const bool is_row = is_row_dir(L->flex_direction);
        const int16_t main_gap = is_row ? edge_or(L->column_gap, L->gap) : edge_or(L->row_gap, L->gap);
        int32_t main_sum = 0;
        int16_t cross_max = 0;
        int count = 0;
        for (uint16_t ct = n->first_child_tag; ct != ER_INVALID_TAG;)
        {
            ERNode* c = er_get_node(ct);
            if (!c)
                break;
            if (c->layout.position != ER_POS_ABSOLUTE && c->layout.display != ER_DISPLAY_NONE)
            {
                int16_t cw = 0, ch = 0;
                measure_content(ct, &cw, &ch);
                const ERLayoutSpec* cl = &c->layout;
                const int16_t outer_w =
                    (int16_t)(cw + edge_or(cl->margin_left, cl->margin) + edge_or(cl->margin_right, cl->margin));
                const int16_t outer_h =
                    (int16_t)(ch + edge_or(cl->margin_top, cl->margin) + edge_or(cl->margin_bottom, cl->margin));
                if (is_row)
                {
                    main_sum += outer_w;
                    if (outer_h > cross_max)
                        cross_max = outer_h;
                }
                else
                {
                    main_sum += outer_h;
                    if (outer_w > cross_max)
                        cross_max = outer_w;
                }
                count++;
            }
            ct = c->next_sibling_tag;
        }
        if (count > 1)
            main_sum += (int32_t)main_gap * (count - 1);
        if (main_sum > INT16_MAX)
            main_sum = INT16_MAX;
        if (is_row)
        {
            content_w = (int16_t)main_sum;
            content_h = cross_max;
        }
        else
        {
            content_h = (int16_t)main_sum;
            content_w = cross_max;
        }
    }

    const ERPadding pad = er_layout_padding(L);

    const int16_t iw = (exp_w != ER_LAYOUT_AUTO) ? exp_w : (int16_t)(content_w + pad.left + pad.right);
    const int16_t ih = (exp_h != ER_LAYOUT_AUTO) ? exp_h : (int16_t)(content_h + pad.top + pad.bottom);
    *out_w = clamp_size(iw, L->min_width, L->max_width);
    *out_h = clamp_size(ih, L->min_height, L->max_height);
    if (cacheable)
    {
        s_measure_cache[tag].w = *out_w;
        s_measure_cache[tag].h = *out_h;
        s_measure_cache[tag].gen = s_measure_gen;
    }
}

/**
 * @brief Recursively computes layout for the subtree rooted at tag.
 *
 * Implements a 7-pass Yoga-compatible algorithm:
 *   Pass 1 — collect in-flow children; determine hypothetical main / cross sizes.
 *   Pass 2 — assign children to wrap lines.
 *   Pass 3 — resolve flex_grow / flex_shrink against each line's free space.
 *   Pass 4 — compute each line's cross-axis size.
 *   Pass 5 — assign main-axis positions (justifyContent) and cross-axis positions (alignSelf).
 *   Pass 6 — write resolved sizes and origins back to children; recurse.
 *   Pass 7 — lay out absolutely positioned children.
 *
 * @param[in] tag  Tag of the node to lay out.
 * @param[in] w    Width allocated to this node in pixels.
 * @param[in] h    Height allocated to this node in pixels.
 * @param[in] x    Screen X origin of this node.
 * @param[in] y    Screen Y origin of this node.
 */
static void compute_layout(const uint16_t tag, const int16_t w, const int16_t h, const int16_t x, const int16_t y)
{
    ERNode* n = er_get_node(tag);
    if (!n)
        return;

    if (n->layout.display == ER_DISPLAY_NONE)
    {
        n->computed.x = 0;
        n->computed.y = 0;
        n->computed.w = 0;
        n->computed.h = 0;
        return;
    }

    n->computed.x = x;
    n->computed.y = y;
    n->computed.w = w;
    n->computed.h = h;

    if (n->first_child_tag == ER_INVALID_TAG)
        return;

    const ERLayoutSpec* L = &n->layout;

    /* Padding (per-edge wins over shorthand). */
    const ERPadding pad = er_layout_padding(L);
    const int16_t pl = pad.left;
    const int16_t pr = pad.right;
    const int16_t pt = pad.top;
    const int16_t pb = pad.bottom;

    int16_t content_w = (int16_t)(w - pl - pr);
    int16_t content_h = (int16_t)(h - pt - pb);
    if (content_w < 0)
        content_w = 0;
    if (content_h < 0)
        content_h = 0;
    const int16_t content_x = (int16_t)(x + pl);
    const int16_t content_y = (int16_t)(y + pt);

    const bool is_row = is_row_dir(L->flex_direction);
    const int16_t main_size = is_row ? content_w : content_h;
    const int16_t cross_avail = is_row ? content_h : content_w;
    const int16_t main_gap = is_row ? edge_or(L->column_gap, L->gap) : edge_or(L->row_gap, L->gap);
    const int16_t cross_gap = is_row ? edge_or(L->row_gap, L->gap) : edge_or(L->column_gap, L->gap);

    /*--------------------------------------------------------------------------
     * Pass 1 — collect in-flow children; assign hypothetical sizes.
     *------------------------------------------------------------------------*/
    int n_inflow = 0;
    for (uint16_t ct = n->first_child_tag; ct != ER_INVALID_TAG;)
    {
        ERNode* c = er_get_node(ct);
        if (!c)
            break;
        if (c->layout.position != ER_POS_ABSOLUTE && c->layout.display != ER_DISPLAY_NONE)
        {
            const ERLayoutSpec* cl = &c->layout;

            /* Intrinsic content size — Text measures its glyph run; containers measure their
             * children (so an auto-sized container fits its content instead of collapsing to 0).
             * Measured lazily below: most children resolve both axes from an explicit size,
             * percentage, or flex_basis and never need it, and measuring recurses the child's
             * whole subtree, so paying for it unconditionally here is exactly the O(depth)
             * remeasurement this cache/laziness combination avoids. */
            bool measured = false;
            int16_t intr_w = 0, intr_h = 0;

            /* Base main size — flex_basis_pct (%) > flex_basis (px) > explicit size > intrinsic. */
            int16_t hypo_main;
            if (cl->flex_basis_pct > 0.0f)
            {
                hypo_main = (int16_t)((float)main_size * cl->flex_basis_pct / 100.0f + 0.5f);
            }
            else if (cl->flex_basis != ER_LAYOUT_AUTO)
            {
                hypo_main = cl->flex_basis;
            }
            else
            {
                const int16_t mainsz = is_row ? cl->width : cl->height;
                const float main_pct = is_row ? cl->width_pct : cl->height_pct;
                if (main_pct > 0.0f)
                    hypo_main = (int16_t)((float)main_size * main_pct / 100.0f + 0.5f);
                else if (mainsz != ER_LAYOUT_AUTO)
                    hypo_main = mainsz;
                else
                {
                    measure_lazy(ct, &measured, &intr_w, &intr_h);
                    hypo_main = is_row ? intr_w : intr_h;
                }
            }
            const int16_t main_mn = is_row ? cl->min_width : cl->min_height;
            const int16_t main_mx = is_row ? cl->max_width : cl->max_height;
            hypo_main = clamp_size(hypo_main, main_mn, main_mx);

            /* Base cross size (stretch may override later if still auto). When aspect_ratio
             * will drive the cross size below, skip measuring: the aspect branch overwrites
             * whatever the intrinsic fallback would have produced anyway. */
            const int16_t crosssz = is_row ? cl->height : cl->width;
            const float cross_pct = is_row ? cl->height_pct : cl->width_pct;
            const int16_t cross_mn = is_row ? cl->min_height : cl->min_width;
            const int16_t cross_mx = is_row ? cl->max_height : cl->max_width;
            const bool cross_via_aspect = cl->aspect_ratio > 0.0f && crosssz == ER_LAYOUT_AUTO && cross_pct <= 0.0f;
            int16_t hypo_cross;
            if (cross_pct > 0.0f)
                hypo_cross = (int16_t)((float)cross_avail * cross_pct / 100.0f + 0.5f);
            else if (crosssz != ER_LAYOUT_AUTO)
                hypo_cross = crosssz;
            else if (cross_via_aspect)
                hypo_cross = 0; /* placeholder — overwritten by the aspect_ratio branch below */
            else
            {
                measure_lazy(ct, &measured, &intr_w, &intr_h);
                hypo_cross = is_row ? intr_h : intr_w;
            }
            hypo_cross = clamp_size(hypo_cross, cross_mn, cross_mx);

            /* aspect_ratio: if the cross dimension is auto (no explicit size or percentage),
             * derive it from the main. aspect_ratio == width / height, so:
             *   row  direction: cross (height) = main (width)  / aspect_ratio
             *   col  direction: cross (width)  = main (height) * aspect_ratio
             */
            if (cross_via_aspect)
            {
                const float new_cross =
                    is_row ? (float)hypo_main / cl->aspect_ratio : (float)hypo_main * cl->aspect_ratio;
                hypo_cross = clamp_size((int16_t)(new_cross + 0.5f), cross_mn, cross_mx);
            }

            /* Per-edge margins. */
            const int16_t ml = edge_or(cl->margin_left, cl->margin);
            const int16_t mr = edge_or(cl->margin_right, cl->margin);
            const int16_t mt = edge_or(cl->margin_top, cl->margin);
            const int16_t mb = edge_or(cl->margin_bottom, cl->margin);

            FlexChild* fc = &s_scratch[n_inflow++];
            fc->tag = ct;
            fc->main = hypo_main;
            fc->cross = hypo_cross;
            fc->margin_main_start = is_row ? ml : mt;
            fc->margin_main_end = is_row ? mr : mb;
            fc->margin_cross_start = is_row ? mt : ml;
            fc->margin_cross_end = is_row ? mb : mr;
            fc->flex_grow = cl->flex_grow;
            fc->flex_shrink = cl->flex_shrink;
            fc->main_min = main_mn;
            fc->main_max = main_mx;
            fc->frozen = 0U;
            fc->line = 0;
            fc->align = (cl->align_self != ER_ALIGN_AUTO) ? cl->align_self : L->align_items;
            if (fc->align == ER_ALIGN_AUTO)
                fc->align = ER_ALIGN_STRETCH;
        }
        ct = c->next_sibling_tag;
    }

    /*--------------------------------------------------------------------------
     * Pass 2 — wrap children into lines.
     *------------------------------------------------------------------------*/
    int n_lines = (n_inflow > 0) ? 1 : 0;
    if (L->flex_wrap != ER_WRAP_NOWRAP && n_inflow > 0)
    {
        int line = 0;
        int16_t line_used = 0;
        bool first_on_line = true;
        for (int i = 0; i < n_inflow; i++)
        {
            const int16_t outer =
                (int16_t)(s_scratch[i].main + s_scratch[i].margin_main_start + s_scratch[i].margin_main_end);
            const int16_t added = (int16_t)(line_used + (first_on_line ? 0 : main_gap) + outer);
            if (!first_on_line && added > main_size)
            {
                line++;
                line_used = outer;
            }
            else
            {
                line_used = added;
                first_on_line = false;
            }
            s_scratch[i].line = (uint8_t)line;
        }
        n_lines = line + 1;
    }

    /*--------------------------------------------------------------------------
     * Pass 3 — per line: resolve flex_grow / flex_shrink against free space.
     *
     * For overflow:scroll containers the main axis is unbounded: children keep
     * their natural sizes instead of growing or shrinking to fit the viewport.
     * This matches React Native's ScrollView behaviour where content overflows
     * the viewport to produce a scrollable virtual content size.
     *------------------------------------------------------------------------*/
    const bool is_scroll = (L->overflow == ER_OVERFLOW_SCROLL);
    if (!is_scroll)
    {
        for (int ln = 0; ln < n_lines; ln++)
        {
            /* Free space against the children's hypothetical (base) main sizes. */
            int32_t base_used = 0;
            int count = 0;
            for (int i = 0; i < n_inflow; i++)
            {
                if (s_scratch[i].line != ln)
                    continue;
                base_used += s_scratch[i].main + s_scratch[i].margin_main_start + s_scratch[i].margin_main_end;
                count++;
            }
            if (count > 1)
                base_used += (count - 1) * main_gap;
            const int32_t free0 = (int32_t)main_size - base_used;
            if (free0 == 0)
                continue;

            /* Positive free space grows (flex_grow); negative shrinks (flex_shrink). Items with
             * no factor in the active direction are frozen at their base size from the start. */
            const bool growing = free0 > 0;
            for (int i = 0; i < n_inflow; i++)
            {
                if (s_scratch[i].line != ln)
                    continue;
                s_scratch[i].frozen =
                    (uint8_t)(growing ? (s_scratch[i].flex_grow == 0) : (s_scratch[i].flex_shrink == 0));
            }

            /* Yoga's resolve-flexible-lengths loop: distribute the remaining free space over the
             * unfrozen items; any that hit a min/max bound freeze at the clamped size and their
             * freed space is redistributed to the rest on the next round. Each round freezes at
             * least one item or commits and exits, so it runs at most `count` times. */
            for (int guard = 0; guard <= count; guard++)
            {
                int32_t used = 0;
                int32_t total_grow = 0;
                int64_t total_shrink_scaled = 0;
                int unfrozen = 0;
                for (int i = 0; i < n_inflow; i++)
                {
                    if (s_scratch[i].line != ln)
                        continue;
                    used += s_scratch[i].main + s_scratch[i].margin_main_start + s_scratch[i].margin_main_end;
                    if (!s_scratch[i].frozen)
                    {
                        total_grow += s_scratch[i].flex_grow;
                        total_shrink_scaled += (int64_t)s_scratch[i].flex_shrink * s_scratch[i].main;
                        unfrozen++;
                    }
                }
                if (count > 1)
                    used += (count - 1) * main_gap;
                const int32_t remaining = (int32_t)main_size - used;

                if (unfrozen == 0 || remaining == 0)
                    break;
                if (growing && total_grow == 0)
                    break;
                if (!growing && total_shrink_scaled == 0)
                    break;

                /* First pass: detect and freeze min/max violations without committing the others. */
                bool froze_one = false;
                for (int i = 0; i < n_inflow; i++)
                {
                    if (s_scratch[i].line != ln || s_scratch[i].frozen)
                        continue;
                    int32_t delta;
                    if (growing)
                        delta = (remaining * s_scratch[i].flex_grow) / total_grow;
                    else
                        delta = (int32_t)(((int64_t)remaining * ((int64_t)s_scratch[i].flex_shrink * s_scratch[i].main))
                                          / total_shrink_scaled);
                    int32_t v = s_scratch[i].main + delta;
                    if (v < 0)
                        v = 0;
                    const int16_t clamped = clamp_size((int16_t)v, s_scratch[i].main_min, s_scratch[i].main_max);
                    if (clamped != v)
                    {
                        s_scratch[i].main = clamped;
                        s_scratch[i].frozen = 1U;
                        froze_one = true;
                    }
                }
                if (froze_one)
                    continue;

                /* No violations: commit the distributed sizes to every remaining flexible item. */
                for (int i = 0; i < n_inflow; i++)
                {
                    if (s_scratch[i].line != ln || s_scratch[i].frozen)
                        continue;
                    int32_t delta;
                    if (growing)
                        delta = (remaining * s_scratch[i].flex_grow) / total_grow;
                    else
                        delta = (int32_t)(((int64_t)remaining * ((int64_t)s_scratch[i].flex_shrink * s_scratch[i].main))
                                          / total_shrink_scaled);
                    int32_t v = s_scratch[i].main + delta;
                    if (v < 0)
                        v = 0;
                    s_scratch[i].main = clamp_size((int16_t)v, s_scratch[i].main_min, s_scratch[i].main_max);
                }
                break;
            }
        }
    }

    /*--------------------------------------------------------------------------
     * Pass 4 — per line: cross-axis size = max child outer cross-size.
     *------------------------------------------------------------------------*/
    for (int ln = 0; ln < n_lines; ln++)
    {
        int16_t maxc = 0;
        for (int i = 0; i < n_inflow; i++)
        {
            if (s_scratch[i].line != ln)
                continue;
            const int16_t outer =
                (int16_t)(s_scratch[i].cross + s_scratch[i].margin_cross_start + s_scratch[i].margin_cross_end);
            if (outer > maxc)
                maxc = outer;
        }
        s_line_cross[ln] = maxc;
    }
    if (L->flex_wrap == ER_WRAP_NOWRAP && n_lines == 1)
        s_line_cross[0] = cross_avail;

    int16_t total_cross = 0;
    for (int ln = 0; ln < n_lines; ln++)
    {
        if (ln > 0)
            total_cross = (int16_t)(total_cross + cross_gap);
        total_cross = (int16_t)(total_cross + s_line_cross[ln]);
    }

    /*--------------------------------------------------------------------------
     * alignContent — distribute leftover cross space among wrap lines.
     *
     * Only meaningful for a multi-line (wrap) container whose lines do not already fill the cross
     * axis. Each mode is one exact fraction: line `ln` starts (ac_a + ln * ac_b) / ac_den past where
     * flex-start would put it, and Pass 5 rounds that once.
     *
     * STRETCH is the same shape — growing every line also pushes each later line's start — so it is
     * just ac_b. The growth is kept separately too, since a line's extent also sizes a stretched
     * child and positions a centred one.
     *------------------------------------------------------------------------*/
    int32_t ac_a = 0;       /**< Numerator of line 0's cross offset. */
    int32_t ac_b = 0;       /**< Numerator added per line. */
    int32_t ac_den = 1;     /**< Shared denominator; always > 0. */
    int32_t ac_stretch = 0; /**< STRETCH: cross space added to EVERY line, over ac_den. */
    if (L->flex_wrap != ER_WRAP_NOWRAP && n_lines > 1)
    {
        int16_t free_cross = (int16_t)(cross_avail - total_cross);
        if (free_cross < 0)
            free_cross = 0;

        if (free_cross > 0)
        {
            switch (L->align_content)
            {
                case ER_ALIGN_CONTENT_FLEX_END:
                    ac_a = free_cross;
                    break;
                case ER_ALIGN_CONTENT_CENTER:
                    ac_a = free_cross;
                    ac_den = 2;
                    break;
                case ER_ALIGN_CONTENT_SPACE_BETWEEN:
                    ac_b = free_cross;
                    ac_den = n_lines - 1;
                    break;
                case ER_ALIGN_CONTENT_SPACE_AROUND:
                    ac_a = free_cross;
                    ac_b = 2 * (int32_t)free_cross;
                    ac_den = 2 * n_lines;
                    break;
                case ER_ALIGN_CONTENT_STRETCH:
                    ac_b = free_cross;
                    ac_den = n_lines;
                    ac_stretch = free_cross;
                    break;
                default: /* ER_ALIGN_CONTENT_FLEX_START — lines packed at the cross-start. */
                    break;
            }
        }
    }

    /* A line's extent grows by whole pixels only; the remainder rides along in the numerators below,
     * so what sits inside a stretched line is still placed exactly. */
    const int16_t ac_stretch_int = (int16_t)(ac_stretch / ac_den);
    const int32_t ac_stretch_rem = ac_stretch - (int32_t)ac_stretch_int * ac_den;

    /*--------------------------------------------------------------------------
     * Pass 5 — main-axis positions (justifyContent) + cross-axis (alignSelf).
     *------------------------------------------------------------------------*/
    const bool rev_main = is_reverse_dir(L->flex_direction);
    const bool rev_cross = (L->flex_wrap == ER_WRAP_WRAP_REVERSE);
    const int32_t cross_den = 2 * ac_den; /* The 2 is alignSelf: center halves the leftover. */
    int32_t line_cross_int = 0;           /* Whole-pixel cross start of line ln (unstretched). */
    for (int ln = 0; ln < n_lines; ln++)
    {
        int32_t line_used = 0;
        int count = 0;
        for (int i = 0; i < n_inflow; i++)
        {
            if (s_scratch[i].line != ln)
                continue;
            line_used += s_scratch[i].main + s_scratch[i].margin_main_start + s_scratch[i].margin_main_end;
            count++;
        }
        if (count > 1)
            line_used += (count - 1) * main_gap;

        const int32_t remaining = (int32_t)main_size - line_used;
        /* An overflowing line keeps the sign for centre and flex-end, so a child bigger than the box
         * overhangs the way it does when it is placed as an absolute. The distribution modes have
         * nothing to spread once the line is full, and fall back to flex-start. */
        const int32_t spread = remaining > 0 ? remaining : 0;

        /* justifyContent as one exact fraction per item: item k sits (j_a + k * j_b) / j_den past
         * where flex-start would have put it. The leading offset and the between-item step share one
         * fraction on purpose — rounding a step and then accumulating it drifts. */
        int32_t j_a = 0;
        int32_t j_b = 0;
        int32_t j_den = 1;
        switch (L->justify_content)
        {
            case ER_JUSTIFY_CENTER:
                j_a = remaining;
                j_den = 2;
                break;
            case ER_JUSTIFY_FLEX_END:
                j_a = remaining;
                break;
            case ER_JUSTIFY_SPACE_BETWEEN:
                if (count > 1)
                {
                    j_b = spread;
                    j_den = count - 1;
                }
                break;
            case ER_JUSTIFY_SPACE_AROUND:
                if (count > 0)
                {
                    /* Half a unit before the first item, a whole unit between each pair. */
                    j_a = spread;
                    j_b = 2 * spread;
                    j_den = 2 * count;
                }
                break;
            case ER_JUSTIFY_SPACE_EVENLY:
                if (count > 0)
                {
                    j_a = spread;
                    j_b = spread;
                    j_den = count + 1;
                }
                break;
            default: /* ER_JUSTIFY_FLEX_START, and SPACE_BETWEEN with nothing to space. */
                break;
        }

        /* Main-axis positions. `cursor` carries only whole pixels — sizes, margins and gaps — so a
         * rounded position never feeds back into where the next sibling lands.
         *
         * A reversed axis is mirrored here, while the offset is still a fraction. Mirroring a rounded
         * position would round the same half pixel twice, in opposite directions. */
        int32_t cursor = 0;
        int32_t k = 0;
        for (int i = 0; i < n_inflow; i++)
        {
            if (s_scratch[i].line != ln)
                continue;

            int32_t pos = cursor + s_scratch[i].margin_main_start;
            int32_t num = j_a + k * j_b;
            if (rev_main)
            {
                /* Mirror the item's OUTER box and re-seat the item inside it, so the margin that
                 * leads is the one on the edge the axis starts from. */
                pos = main_size - cursor - s_scratch[i].margin_main_end - s_scratch[i].main;
                num = -num;
            }
            s_scratch[i].main_pos = (int16_t)(pos + div_round(num, j_den));

            cursor +=
                (int32_t)s_scratch[i].margin_main_start + s_scratch[i].main + s_scratch[i].margin_main_end + main_gap;
            k++;
        }

        /* Cross-axis positions via alignSelf. Three fractions share cross_den — the line's
         * alignContent offset, the remainder of a stretched line's growth, and the half that centring
         * takes — and are rounded together at the end of the loop body. */
        const int16_t this_cross = (int16_t)(s_line_cross[ln] + ac_stretch_int);
        const int32_t line_num = 2 * (ac_a + (int32_t)ln * ac_b);
        for (int i = 0; i < n_inflow; i++)
        {
            if (s_scratch[i].line != ln)
                continue;

            int16_t inner = (int16_t)(this_cross - s_scratch[i].margin_cross_start - s_scratch[i].margin_cross_end);
            if (inner < 0)
                inner = 0;

            int32_t pos = s_scratch[i].margin_cross_start; /* Whole pixels, from the line's start. */
            int32_t num = 0;                               /* Sub-pixel remainder, over cross_den. */
            switch (s_scratch[i].align)
            {
                case ER_ALIGN_STRETCH:
                {
                    /* Stretch fills cross-axis only when size was auto (no explicit px or %). */
                    const ERNode* c = er_get_node(s_scratch[i].tag);
                    if (c)
                    {
                        const int16_t cz = is_row ? c->layout.height : c->layout.width;
                        const float cz_pct = is_row ? c->layout.height_pct : c->layout.width_pct;
                        if (cz == ER_LAYOUT_AUTO && cz_pct <= 0.0f)
                        {
                            const int16_t mn = is_row ? c->layout.min_height : c->layout.min_width;
                            const int16_t mx = is_row ? c->layout.max_height : c->layout.max_width;
                            s_scratch[i].cross = clamp_size(inner, mn, mx);
                        }
                    }
                    break;
                }
                case ER_ALIGN_FLEX_START:
                    break;
                case ER_ALIGN_CENTER:
                    num = (int32_t)(inner - s_scratch[i].cross) * ac_den + ac_stretch_rem;
                    break;
                case ER_ALIGN_FLEX_END:
                    pos = this_cross - s_scratch[i].margin_cross_end - s_scratch[i].cross;
                    num = 2 * ac_stretch_rem;
                    break;
                default:
                    break;
            }

            pos += line_cross_int;
            num += line_num;

            /* wrap-reverse mirrors about the CONTENT box, not about the lines' own extent, which
             * would reverse their order without moving the block off the cross-start. alignContent
             * has already placed the block, so the mirror just carries it along. Lines that overflow
             * mirror to negative positions rather than clamping. Mirror before rounding, as above. */
            if (rev_cross)
            {
                pos = cross_avail - pos - s_scratch[i].cross;
                num = -num;
            }
            s_scratch[i].cross_pos = (int16_t)(pos + div_round(num, cross_den));
        }

        /* ac_b carries the stretch growth, so this advances by the line's unstretched extent. */
        line_cross_int += (int32_t)s_line_cross[ln] + cross_gap;
    }

    /*--------------------------------------------------------------------------
     * Pass 6 — write resolved sizes and origins back to children; recurse.
     *
     * Two-loop design: Pass 6a commits every child's resolved rect to the
     * child's own computed struct before any recursion.  Pass 6b then walks
     * the sibling chain to recurse — it never reads s_scratch[i].tag again,
     * so the recursive calls' use of the global s_scratch cannot corrupt the
     * parent's in-progress child list.
     *------------------------------------------------------------------------*/

    /* Pass 6a — resolve and store each in-flow child's rect; no recursion. */
    for (int i = 0; i < n_inflow; i++)
    {
        ERNode* c = er_get_node(s_scratch[i].tag);
        if (!c)
            continue;

        int16_t cx, cy, cw, ch;
        if (is_row)
        {
            cx = (int16_t)(content_x + s_scratch[i].main_pos);
            cy = (int16_t)(content_y + s_scratch[i].cross_pos);
            cw = s_scratch[i].main;
            ch = s_scratch[i].cross;
        }
        else
        {
            cx = (int16_t)(content_x + s_scratch[i].cross_pos);
            cy = (int16_t)(content_y + s_scratch[i].main_pos);
            cw = s_scratch[i].cross;
            ch = s_scratch[i].main;
        }

        /* Relative-position offsets (left/right/top/bottom shift in flow). */
        const ERLayoutSpec* cl = &c->layout;
        if (cl->left != ER_LAYOUT_AUTO)
            cx = (int16_t)(cx + cl->left);
        else if (cl->right != ER_LAYOUT_AUTO)
            cx = (int16_t)(cx - cl->right);
        if (cl->top != ER_LAYOUT_AUTO)
            cy = (int16_t)(cy + cl->top);
        else if (cl->bottom != ER_LAYOUT_AUTO)
            cy = (int16_t)(cy - cl->bottom);

        c->computed.x = cx;
        c->computed.y = cy;
        c->computed.w = cw;
        c->computed.h = ch;
    }

    /* Pass 6b — recurse via sibling chain using pre-stored computed rects. A display:none child was
     * excluded from every sizing pass above, so its computed rect still holds whatever it had when it
     * was last visible; run it through compute_layout anyway (which zeroes the box and stops without
     * recursing) so a hidden node reports the empty layout it actually occupies — onLayout included —
     * rather than a stale one. Absolutely-positioned children are otherwise handled by pass 7, but a
     * hidden one is collapsed here since pass 7 skips it too. */
    for (uint16_t ct = n->first_child_tag; ct != ER_INVALID_TAG;)
    {
        ERNode* c = er_get_node(ct);
        if (!c)
            break;
        if (c->layout.display == ER_DISPLAY_NONE)
            compute_layout(c->tag, 0, 0, 0, 0);
        else if (c->layout.position != ER_POS_ABSOLUTE)
            compute_layout(c->tag, c->computed.w, c->computed.h, c->computed.x, c->computed.y);
        ct = c->next_sibling_tag;
    }

    /*--------------------------------------------------------------------------
     * Pass 7 — absolutely positioned children.
     *------------------------------------------------------------------------*/
    for (uint16_t ct = n->first_child_tag; ct != ER_INVALID_TAG;)
    {
        ERNode* c = er_get_node(ct);
        if (!c)
            break;
        if (c->layout.position == ER_POS_ABSOLUTE && c->layout.display != ER_DISPLAY_NONE)
        {
            const ERLayoutSpec* cl = &c->layout;
            const int16_t ml = edge_or(cl->margin_left, cl->margin);
            const int16_t mr = edge_or(cl->margin_right, cl->margin);
            const int16_t mt = edge_or(cl->margin_top, cl->margin);
            const int16_t mb = edge_or(cl->margin_bottom, cl->margin);

            /* The containing block is the parent's PADDING box -- this node's own border box, since
             * layout reserves no border width -- NOT the content box the flow children
             * are placed in. That is what CSS and React Native resolve against: `left: 0` sits at the
             * padding edge and ignores the parent's padding, and a percentage is a fraction of the
             * whole box. Padding returns only for an axis with no inset at all, whose
             * static position is the flow position the child would have had.
             *
             * Size each axis from whatever the style actually pins it to: an explicit length, a
             * percentage of the containing block, or a pair of opposing insets. An axis none of those
             * answer is left unresolved for the aspect-ratio and content fallbacks below. */
            int16_t cw = 0, ach = 0;
            bool have_w = true, have_h = true;
            if (cl->width != ER_LAYOUT_AUTO)
                cw = cl->width;
            else if (cl->width_pct > 0.0f)
                cw = (int16_t)((float)w * cl->width_pct / 100.0f + 0.5f);
            else if (cl->left != ER_LAYOUT_AUTO && cl->right != ER_LAYOUT_AUTO)
                cw = (int16_t)(w - cl->left - cl->right - ml - mr);
            else
                have_w = false;

            if (cl->height != ER_LAYOUT_AUTO)
                ach = cl->height;
            else if (cl->height_pct > 0.0f)
                ach = (int16_t)((float)h * cl->height_pct / 100.0f + 0.5f);
            else if (cl->top != ER_LAYOUT_AUTO && cl->bottom != ER_LAYOUT_AUTO)
                ach = (int16_t)(h - cl->top - cl->bottom - mt - mb);
            else
                have_h = false;

            /* aspect_ratio (= width / height) fills an axis from the one already resolved, exactly as
             * Pass 1 derives a flow child's cross size from its main. */
            if (cl->aspect_ratio > 0.0f && have_w != have_h)
            {
                if (have_w)
                {
                    ach = (int16_t)((float)cw / cl->aspect_ratio + 0.5f);
                    have_h = true;
                }
                else
                {
                    cw = (int16_t)((float)ach * cl->aspect_ratio + 0.5f);
                    have_w = true;
                }
            }

            /* Still unresolved: size to content, the same max-content measurement a flow child falls
             * back on, instead of collapsing to 0. A zero-sized box paints its children fine (nothing
             * clips them), so the collapse was invisible on screen but fatal to input: hit_test_node
             * gates entry on the node's own rect, so an empty one turned the whole subtree into a dead
             * zone and let every touch fall through to what was behind it. measure_content
             * is memoised per layout pass, so this costs one measurement per absolute node. */
            if (!have_w || !have_h)
            {
                int16_t intr_w = 0, intr_h = 0;
                measure_content(ct, &intr_w, &intr_h);
                if (!have_w)
                    cw = intr_w;
                if (!have_h)
                    ach = intr_h;
            }

            cw = clamp_size(cw, cl->min_width, cl->max_width);
            ach = clamp_size(ach, cl->min_height, cl->max_height);

            /* An axis with NO inset falls back to its static position: where this child would have sat
             * had it stayed in flow, which is inside the CONTENT box and honours the parent's
             * justifyContent / alignItems (the child's own alignSelf winning, as in Pass 1). Resolved
             * per axis, so `left: 10` with no `top` pins x and still aligns y. */
            uint8_t self_align = (cl->align_self != ER_ALIGN_AUTO) ? cl->align_self : L->align_items;
            if (self_align == ER_ALIGN_AUTO)
                self_align = ER_ALIGN_STRETCH;

            const int16_t s_main = is_row ? cw : ach;
            const int16_t s_cross = is_row ? ach : cw;
            const int16_t mm_lead = is_row ? ml : mt;
            const int16_t mm_trail = is_row ? mr : mb;
            const int16_t mc_lead = is_row ? mt : ml;
            const int16_t mc_trail = is_row ? mb : mr;

            const int16_t main_static =
                solo_axis_pos(is_row ? content_x : content_y,
                              main_size,
                              s_main,
                              mm_lead,
                              mm_trail,
                              solo_main_offset2(L->justify_content, (int16_t)(main_size - s_main - mm_lead - mm_trail)),
                              is_reverse_dir(L->flex_direction));
            const int16_t cross_static =
                solo_axis_pos(is_row ? content_y : content_x,
                              cross_avail,
                              s_cross,
                              mc_lead,
                              mc_trail,
                              solo_cross_offset2(self_align, (int16_t)(cross_avail - s_cross - mc_lead - mc_trail)),
                              L->flex_wrap == ER_WRAP_WRAP_REVERSE);

            /* Insets measure from the padding box; an axis pinned by neither takes the static position. */
            int16_t cx;
            if (cl->left != ER_LAYOUT_AUTO)
                cx = (int16_t)(x + cl->left + ml);
            else if (cl->right != ER_LAYOUT_AUTO)
                cx = (int16_t)(x + w - cl->right - cw - mr);
            else
                cx = is_row ? main_static : cross_static;

            int16_t cy;
            if (cl->top != ER_LAYOUT_AUTO)
                cy = (int16_t)(y + cl->top + mt);
            else if (cl->bottom != ER_LAYOUT_AUTO)
                cy = (int16_t)(y + h - cl->bottom - ach - mb);
            else
                cy = is_row ? cross_static : main_static;

            compute_layout(ct, cw, ach, cx, cy);
        }
        ct = c->next_sibling_tag;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_layout_compute(uint16_t root_tag, int16_t w, int16_t h)
{
    /* Advance the measure-cache generation so every measure_content() call this pass
     * starts with a clean slate. gen 0 is reserved as "never valid" (a fresh
     * s_measure_cache is zero-initialized), so skip it on wraparound; a wrap also means
     * every table slot could coincidentally hold stale gen 1 already, so clear it. */
    s_measure_gen++;
    if (s_measure_gen == 0U)
    {
        memset(s_measure_cache, 0, sizeof(s_measure_cache));
        s_measure_gen = 1U;
    }
    compute_layout(root_tag, w, h, 0, 0);
}
