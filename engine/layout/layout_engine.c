#include "layout_engine.h"
#include "text_renderer.h"
#include <stdint.h>

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

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static FlexChild s_scratch[ERUI_MAX_NODES];
static int16_t s_line_cross[ERUI_MAX_NODES];

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

    /* Text leaf: measure the glyph run (mirrors the Text branch of Pass 1). */
    if (n->type == ER_NODE_TEXT)
    {
        int measured_w = 0, measured_h = 0;
        er_text_measure(n->props.text.text,
                        n->props.text.font_size,
                        n->props.text.font_family,
                        n->props.text.letter_spacing,
                        &measured_w,
                        &measured_h);
        const int16_t line_h = (n->props.text.line_height > 0) ? n->props.text.line_height : (int16_t)measured_h;
        const int lines = (n->props.text.number_of_lines > 1) ? (int)n->props.text.number_of_lines : 1;
        const int16_t tw = (exp_w != ER_LAYOUT_AUTO) ? exp_w : (int16_t)measured_w;
        const int16_t th = (exp_h != ER_LAYOUT_AUTO) ? exp_h : (int16_t)(line_h * lines);
        *out_w = clamp_size(tw, L->min_width, L->max_width);
        *out_h = clamp_size(th, L->min_height, L->max_height);
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

    const int16_t pl = edge_or(L->padding_left, L->padding);
    const int16_t pr = edge_or(L->padding_right, L->padding);
    const int16_t pt = edge_or(L->padding_top, L->padding);
    const int16_t pb = edge_or(L->padding_bottom, L->padding);

    const int16_t iw = (exp_w != ER_LAYOUT_AUTO) ? exp_w : (int16_t)(content_w + pl + pr);
    const int16_t ih = (exp_h != ER_LAYOUT_AUTO) ? exp_h : (int16_t)(content_h + pt + pb);
    *out_w = clamp_size(iw, L->min_width, L->max_width);
    *out_h = clamp_size(ih, L->min_height, L->max_height);
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
    const int16_t pl = edge_or(L->padding_left, L->padding);
    const int16_t pr = edge_or(L->padding_right, L->padding);
    const int16_t pt = edge_or(L->padding_top, L->padding);
    const int16_t pb = edge_or(L->padding_bottom, L->padding);

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
             * children (so an auto-sized container fits its content instead of collapsing to 0). */
            int16_t intr_w = 0, intr_h = 0;
            measure_content(ct, &intr_w, &intr_h);

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
                if (mainsz != ER_LAYOUT_AUTO)
                    hypo_main = mainsz;
                else
                    hypo_main = is_row ? intr_w : intr_h;
            }
            const int16_t main_mn = is_row ? cl->min_width : cl->min_height;
            const int16_t main_mx = is_row ? cl->max_width : cl->max_height;
            hypo_main = clamp_size(hypo_main, main_mn, main_mx);

            /* Base cross size (stretch may override later if still auto). */
            const int16_t crosssz = is_row ? cl->height : cl->width;
            const int16_t cross_mn = is_row ? cl->min_height : cl->min_width;
            const int16_t cross_mx = is_row ? cl->max_height : cl->max_width;
            int16_t hypo_cross;
            if (crosssz != ER_LAYOUT_AUTO)
                hypo_cross = crosssz;
            else
                hypo_cross = is_row ? intr_h : intr_w;
            hypo_cross = clamp_size(hypo_cross, cross_mn, cross_mx);

            /* aspect_ratio: if the cross dimension is auto, derive it from the main.
             * aspect_ratio == width / height, so:
             *   row  direction: cross (height) = main (width)  / aspect_ratio
             *   col  direction: cross (width)  = main (height) * aspect_ratio
             */
            if (cl->aspect_ratio > 0.0f && crosssz == ER_LAYOUT_AUTO)
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
     * Only meaningful for a multi-line (wrap) container whose lines do not already fill the
     * cross axis. STRETCH grows each line equally; the other modes add a leading offset
     * (ac_offset) and/or extra spacing between lines (ac_between) consumed in Pass 5.
     *------------------------------------------------------------------------*/
    int16_t ac_offset = 0;
    int16_t ac_between = 0;
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
                    ac_offset = free_cross;
                    break;
                case ER_ALIGN_CONTENT_CENTER:
                    ac_offset = (int16_t)(free_cross / 2);
                    break;
                case ER_ALIGN_CONTENT_SPACE_BETWEEN:
                    ac_between = (int16_t)(free_cross / (n_lines - 1));
                    break;
                case ER_ALIGN_CONTENT_SPACE_AROUND:
                {
                    const int16_t unit = (int16_t)(free_cross / n_lines);
                    ac_offset = (int16_t)(unit / 2);
                    ac_between = unit;
                    break;
                }
                case ER_ALIGN_CONTENT_STRETCH:
                {
                    const int16_t add = (int16_t)(free_cross / n_lines);
                    for (int ln = 0; ln < n_lines; ln++)
                        s_line_cross[ln] = (int16_t)(s_line_cross[ln] + add);
                    total_cross = (int16_t)(total_cross + (int16_t)(add * n_lines));
                    break;
                }
                default: /* ER_ALIGN_CONTENT_FLEX_START — lines packed at the cross-start. */
                    break;
            }
        }
    }

    /*--------------------------------------------------------------------------
     * Pass 5 — main-axis positions (justifyContent) + cross-axis (alignSelf).
     *------------------------------------------------------------------------*/
    int16_t line_cross_offset = ac_offset;
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

        int32_t remaining = (int32_t)main_size - line_used;
        if (remaining < 0)
            remaining = 0;

        int16_t main_offset = 0;
        int16_t between = main_gap;
        switch (L->justify_content)
        {
            case ER_JUSTIFY_FLEX_START:
                break;
            case ER_JUSTIFY_CENTER:
                main_offset = (int16_t)(remaining / 2);
                break;
            case ER_JUSTIFY_FLEX_END:
                main_offset = (int16_t)remaining;
                break;
            case ER_JUSTIFY_SPACE_BETWEEN:
                if (count > 1)
                    between = (int16_t)(main_gap + remaining / (count - 1));
                break;
            case ER_JUSTIFY_SPACE_AROUND:
                if (count > 0)
                {
                    const int32_t unit = remaining / count;
                    main_offset = (int16_t)(unit / 2);
                    between = (int16_t)(main_gap + unit);
                }
                break;
            case ER_JUSTIFY_SPACE_EVENLY:
                if (count > 0)
                {
                    const int32_t unit = remaining / (count + 1);
                    main_offset = (int16_t)unit;
                    between = (int16_t)(main_gap + unit);
                }
                break;
            default:
                break;
        }

        /* Main-axis positions in natural order. */
        int16_t cursor = main_offset;
        for (int i = 0; i < n_inflow; i++)
        {
            if (s_scratch[i].line != ln)
                continue;
            cursor = (int16_t)(cursor + s_scratch[i].margin_main_start);
            s_scratch[i].main_pos = cursor;
            cursor = (int16_t)(cursor + s_scratch[i].main + s_scratch[i].margin_main_end + between);
        }

        /* Reverse the main axis if requested. */
        if (is_reverse_dir(L->flex_direction))
        {
            for (int i = 0; i < n_inflow; i++)
            {
                if (s_scratch[i].line != ln)
                    continue;
                const int16_t end = (int16_t)(s_scratch[i].main_pos + s_scratch[i].main);
                s_scratch[i].main_pos = (int16_t)(main_size - end);
            }
        }

        /* Cross-axis positions via alignSelf. */
        const int16_t this_cross = s_line_cross[ln];
        for (int i = 0; i < n_inflow; i++)
        {
            if (s_scratch[i].line != ln)
                continue;

            int16_t inner = (int16_t)(this_cross - s_scratch[i].margin_cross_start - s_scratch[i].margin_cross_end);
            if (inner < 0)
                inner = 0;

            switch (s_scratch[i].align)
            {
                case ER_ALIGN_STRETCH:
                {
                    /* Stretch fills cross-axis only when size was auto. */
                    const ERNode* c = er_get_node(s_scratch[i].tag);
                    if (c)
                    {
                        const int16_t cz = is_row ? c->layout.height : c->layout.width;
                        if (cz == ER_LAYOUT_AUTO)
                        {
                            const int16_t mn = is_row ? c->layout.min_height : c->layout.min_width;
                            const int16_t mx = is_row ? c->layout.max_height : c->layout.max_width;
                            s_scratch[i].cross = clamp_size(inner, mn, mx);
                        }
                    }
                    s_scratch[i].cross_pos = s_scratch[i].margin_cross_start;
                    break;
                }
                case ER_ALIGN_FLEX_START:
                    s_scratch[i].cross_pos = s_scratch[i].margin_cross_start;
                    break;
                case ER_ALIGN_CENTER:
                    s_scratch[i].cross_pos =
                        (int16_t)(s_scratch[i].margin_cross_start + (inner - s_scratch[i].cross) / 2);
                    break;
                case ER_ALIGN_FLEX_END:
                    s_scratch[i].cross_pos = (int16_t)(this_cross - s_scratch[i].margin_cross_end - s_scratch[i].cross);
                    break;
                default:
                    s_scratch[i].cross_pos = s_scratch[i].margin_cross_start;
                    break;
            }
            s_scratch[i].cross_pos = (int16_t)(s_scratch[i].cross_pos + line_cross_offset);
        }

        line_cross_offset = (int16_t)(line_cross_offset + this_cross + cross_gap + ac_between);
    }

    /* wrap-reverse: mirror all lines along the cross-axis. */
    if (L->flex_wrap == ER_WRAP_WRAP_REVERSE && n_inflow > 0)
    {
        for (int i = 0; i < n_inflow; i++)
        {
            const int16_t end = (int16_t)(s_scratch[i].cross_pos + s_scratch[i].cross);
            s_scratch[i].cross_pos = (int16_t)(total_cross - end);
        }
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

    /* Pass 6b — recurse via sibling chain using pre-stored computed rects. */
    for (uint16_t ct = n->first_child_tag; ct != ER_INVALID_TAG;)
    {
        ERNode* c = er_get_node(ct);
        if (!c)
            break;
        if (c->layout.position != ER_POS_ABSOLUTE && c->layout.display != ER_DISPLAY_NONE)
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

            int16_t cw;
            if (cl->width != ER_LAYOUT_AUTO)
                cw = cl->width;
            else if (cl->left != ER_LAYOUT_AUTO && cl->right != ER_LAYOUT_AUTO)
                cw = (int16_t)(content_w - cl->left - cl->right - ml - mr);
            else
                cw = 0;
            cw = clamp_size(cw, cl->min_width, cl->max_width);

            int16_t ach;
            if (cl->height != ER_LAYOUT_AUTO)
                ach = cl->height;
            else if (cl->top != ER_LAYOUT_AUTO && cl->bottom != ER_LAYOUT_AUTO)
                ach = (int16_t)(content_h - cl->top - cl->bottom - mt - mb);
            else
                ach = 0;
            ach = clamp_size(ach, cl->min_height, cl->max_height);

            int16_t cx;
            if (cl->left != ER_LAYOUT_AUTO)
                cx = (int16_t)(content_x + cl->left + ml);
            else if (cl->right != ER_LAYOUT_AUTO)
                cx = (int16_t)(content_x + content_w - cl->right - cw - mr);
            else
                cx = (int16_t)(content_x + ml);

            int16_t cy;
            if (cl->top != ER_LAYOUT_AUTO)
                cy = (int16_t)(content_y + cl->top + mt);
            else if (cl->bottom != ER_LAYOUT_AUTO)
                cy = (int16_t)(content_y + content_h - cl->bottom - ach - mb);
            else
                cy = (int16_t)(content_y + mt);

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
    compute_layout(root_tag, w, h, 0, 0);
}
