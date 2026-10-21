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
#include "er_limits.h" /* ERUI_MAX_NODES — the same pool compositor.c allocates and hit_test.c walks */
#include "text_renderer.h"
#include <stdint.h>
#include <string.h>

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
    int16_t main_min;   /**< Main-axis min constraint (ER_LAYOUT_AUTO = none). */
    int16_t main_max;   /**< Main-axis max constraint (ER_LAYOUT_AUTO = none). */
    int16_t measured_w; /**< Row item sized by its content's height: the width measured at (else AUTO). */
    uint8_t line;       /**< Wrap-line index. */
    uint8_t align;      /**< Resolved align (auto → parent align_items). */
    uint8_t frozen;     /**< Pass 3: 1 once the item's flexed main size is final. */
} FlexChild;

/**
 * @brief Per-pass memoisation slots for measure_content(), keyed by node tag.
 *
 * measure_content() depends only on that node's own subtree, which nothing mutates during
 * a layout pass, and on the width it is offered — so for a given (tag, avail) pair its result
 * is a constant for the whole er_layout_compute() call. Without this cache, compute_layout()
 * re-measures every child once per Pass 1 it participates in *and* recurses into that
 * same child, so a leaf at ancestor depth d gets remeasured d times. The `gen` tags make
 * the cache self-invalidating across passes without a memset: a slot is only valid when
 * its gen matches the current pass's s_measure_gen.
 *
 * A row measures a child twice — at most the row's width for its flex basis, then at the width
 * flexing gives it — so each node keeps its two most recent measurements (MEASURE_CACHE_WAYS).
 * With one, the second would evict the first, and the row's own layout pass, which asks for both
 * again, would measure the child's whole subtree twice more.
 */
typedef struct
{
    int16_t w, h;
    int16_t avail; /**< Width the cached size was measured against (ER_LAYOUT_AUTO = unconstrained). */
    uint16_t gen;
} MeasureCacheEntry;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static FlexChild s_scratch[ERUI_MAX_NODES];
static int16_t s_line_cross[ERUI_MAX_NODES];
/** @brief Measurements kept per node; way 0 is the most recent. */
#define MEASURE_CACHE_WAYS 2

static MeasureCacheEntry s_measure_cache[ERUI_MAX_NODES][MEASURE_CACHE_WAYS];
static uint16_t s_measure_gen; /**< Current layout pass id; 0 means "no pass has run yet". */

/**
 * First s_scratch slot measure_content() has claimed. It takes its rows' items from the TOP of the
 * array, downwards, while compute_layout() fills it from the bottom: every slot in use belongs to a
 * different node, so the two never meet in a pool of ERUI_MAX_NODES.
 */
static int s_measure_top = ERUI_MAX_NODES;

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
 * @brief Clamps a fractional size to [mn, mx], treating ER_LAYOUT_AUTO bounds as unconstrained.
 *
 * The float twin of clamp_size(), for the one place a size is still fractional: an absolute's size can
 * be a percentage, or the gap left between two percentage insets, and it is not rounded until both of
 * its edges are — clamping it as an integer here would round it a second time.
 *
 * @param[in] v   Value to clamp.
 * @param[in] mn  Minimum bound (ER_LAYOUT_AUTO = no minimum).
 * @param[in] mx  Maximum bound (ER_LAYOUT_AUTO = no maximum).
 *
 * @return The clamped value, never negative.
 */
static float clamp_sizef(float v, const int16_t mn, const int16_t mx)
{
    /* Settled first, because every comparison below is false for a NaN — it would sail through the
     * clamps untouched and break the contract this function exists to enforce. */
    if (v != v)
        v = 0.0f;
    if (mn != ER_LAYOUT_AUTO && v < (float)mn)
        v = (float)mn;
    if (mx != ER_LAYOUT_AUTO && v > (float)mx)
        v = (float)mx;
    if (v < 0.0f)
        v = 0.0f;
    return v;
}

/**
 * @brief Rounds a fractional pixel coordinate half up, the way div_round() rounds an exact fraction.
 *
 * `(int)(v + 0.5f)` truncates toward zero, which rounds a negative coordinate the wrong way — and a
 * coordinate here is routinely negative, since an inset may be negative and an absolute is free to sit
 * outside its containing block.
 *
 * @param[in] v  Coordinate in pixels.
 *
 * @return floor(v + 1/2).
 */
static int16_t round_px(const float v)
{
    /* Float-to-int is undefined past int16 and for a NaN, and both are reachable from a style: a
     * percentage is spelled as text, so `'NaN%'` parses and `'100000%'` of a full screen does not fit.
     * Settle them here rather than letting a coordinate wrap into the opposite corner. */
    if (v != v)
        return 0;
    if (v >= (float)INT16_MAX)
        return INT16_MAX;
    if (v <= (float)INT16_MIN)
        return INT16_MIN;

    const float t = v + 0.5f;
    int32_t i = (int32_t)t;
    if ((float)i > t)
        i--;
    return (int16_t)i;
}

/**
 * @brief The whole-pixel distance between two rounded edges — a size on the pixel grid.
 *
 * round_px() clamps each edge into int16 separately, so a wild percentage can saturate one edge to each
 * end of that range and leave a difference twice as wide as an int16, or a reversed one. A size is
 * never negative and never leaves int16, so it is settled here rather than wrapping on the cast.
 *
 * @param[in] lo  Rounded near edge.
 * @param[in] hi  Rounded far edge.
 *
 * @return hi - lo, clamped to [0, INT16_MAX].
 */
static int16_t span_px(const int16_t lo, const int16_t hi)
{
    const int32_t d = (int32_t)hi - (int32_t)lo;
    if (d < 0)
        return 0;
    if (d > INT16_MAX)
        return INT16_MAX;
    return (int16_t)d;
}

/**
 * @brief Resolves one inset edge — left, top, right or bottom — to pixels.
 *
 * A percentage measures against `base`, the containing block's extent on the edge's OWN axis: left and
 * right take its width, top and bottom its height. It wins over the pixel field the way width_pct wins
 * over width, and may be negative. `0.0` is the "not set" sentinel, so the bindings lower an authored
 * `0%` onto the PIXEL field instead — 0% of any box is 0 px, so it pins the edge either way.
 * A NaN percentage is not a value and reads as unset.
 *
 * The result stays fractional so the caller can round the edge it lands on exactly once.
 *
 * @param[in]  px    Pixel field (ER_LAYOUT_AUTO = not set).
 * @param[in]  pct   Percentage field (0.0 = not set).
 * @param[in]  base  Containing-block extent the percentage measures against.
 * @param[out] out   Receives the resolved inset; untouched when the edge is not set.
 *
 * @return true when the edge is set.
 */
static bool inset_px(const int16_t px, const float pct, const int16_t base, float* out)
{
    /* `pct == pct` is the NaN test: a percentage is spelled as text, so `'NaN%'` parses. A NaN is not a
     * value, so the edge falls back to the pixel field rather than poisoning the whole axis — a NaN
     * position would take the node's size down with it. */
    if (pct != 0.0f && pct == pct)
    {
        *out = (float)base * pct / 100.0f;
        return true;
    }
    if (px == ER_LAYOUT_AUTO)
        return false;
    *out = (float)px;
    return true;
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
 * @brief Rounds a cumulative edge held as `carry + rem / den` to a whole pixel.
 *
 * Sizes ride the same pixel grid as positions: an item spans from one rounded edge to the next, so a
 * leftover that does not divide evenly lands on whichever item straddles the half pixel instead of
 * being dropped at the end. Splitting the edge into a whole part and a fraction keeps the running sum
 * small enough to stay exact no matter how many items share the line.
 *
 * @param[in] carry     Whole pixels of the edge.
 * @param[in] rem       Fraction of a pixel still outstanding, in [0, den).
 * @param[in] den       Denominator of the fraction. Must be > 0.
 * @param[in] reversed  Whether the axis measures this edge from its far end.
 *
 * @return The edge rounded to a whole pixel.
 */
static int32_t edge_round(const int32_t carry, const int64_t rem, const int64_t den, const bool reversed)
{
    /* A reversed axis mirrors the edge before rounding it, and mirroring flips which way a value
     * sitting exactly on a half pixel goes: round(C - v) is C - v rounded DOWN there. */
    const int64_t twice = 2 * rem;
    return carry + ((reversed ? (twice > den) : (twice >= den)) ? 1 : 0);
}

/**
 * @brief One flex item's exact share of a line's free space, as a numerator over a shared denominator.
 *
 * The denominator is the line's `total_grow` when growing and its `total_shrink_scaled` when
 * shrinking, so every item on the line is measured against the same scale and their shares add up
 * exactly. Yoga carries this as a float all the way to the end; keeping it as a fraction is how the
 * integer engine does the same.
 *
 * @param[in] c          The item.
 * @param[in] remaining  Free main-axis space on the line: positive when growing, negative when shrinking.
 * @param[in] growing    Whether the line is growing (flex_grow) rather than shrinking (flex_shrink).
 *
 * @return The share's numerator. Negative when shrinking.
 */
static int64_t flex_share_num(const FlexChild* c, const int32_t remaining, const bool growing)
{
    if (growing)
        return (int64_t)remaining * c->flex_grow;
    return (int64_t)remaining * ((int64_t)c->flex_shrink * c->main);
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

/**
 * @brief Resolves flex_grow / flex_shrink for the items of one line against its free space.
 *
 * Yoga's resolve-flexible-lengths loop: distribute the remaining free space over the unfrozen
 * items; any that hit a min/max bound freeze at the clamped size and their freed space is
 * redistributed to the rest on the next round. Shared by layout (Pass 3) and by measure_content(),
 * which has to flex a row to learn how tall its children are at the widths they end up with.
 *
 * @param[in,out] items      Flex items; `main` holds each base size on entry and the flexed size on return.
 * @param[in]     n          Number of entries in items.
 * @param[in]     ln         Line to resolve; items on other lines are left alone.
 * @param[in]     main_size  Content-box extent of the container on the main axis.
 * @param[in]     main_gap   Gap between adjacent items on the main axis.
 * @param[in]     rev_main   Whether the main axis is reversed (it decides which way a half pixel rounds).
 */
static void resolve_flex_line(
    FlexChild* items, const int n, const int ln, const int16_t main_size, const int16_t main_gap, const bool rev_main)
{
    /* Free space against the children's hypothetical (base) main sizes. */
    int32_t base_used = 0;
    int count = 0;
    for (int i = 0; i < n; i++)
    {
        if (items[i].line != ln)
            continue;
        base_used += items[i].main + items[i].margin_main_start + items[i].margin_main_end;
        count++;
    }
    if (count > 1)
        base_used += (count - 1) * main_gap;
    const int32_t free0 = (int32_t)main_size - base_used;
    if (free0 == 0)
        return;

    /* Positive free space grows (flex_grow); negative shrinks (flex_shrink). Items with
     * no factor in the active direction are frozen at their base size from the start. */
    const bool growing = free0 > 0;
    for (int i = 0; i < n; i++)
    {
        if (items[i].line != ln)
            continue;
        items[i].frozen = (uint8_t)(growing ? (items[i].flex_grow == 0) : (items[i].flex_shrink == 0));
    }

    /* Each round freezes at least one item or commits and exits, so it runs at most `count` times. */
    for (int guard = 0; guard <= count; guard++)
    {
        int32_t used = 0;
        int32_t total_grow = 0;
        int64_t total_shrink_scaled = 0;
        int unfrozen = 0;
        for (int i = 0; i < n; i++)
        {
            if (items[i].line != ln)
                continue;
            used += items[i].main + items[i].margin_main_start + items[i].margin_main_end;
            if (!items[i].frozen)
            {
                total_grow += items[i].flex_grow;
                total_shrink_scaled += (int64_t)items[i].flex_shrink * items[i].main;
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

        /* Shares stay exact fractions over one denominator per line, so neither loop below
         * truncates: a size is only ever rounded once, off the pixel grid. */
        const int64_t den = growing ? (int64_t)total_grow : total_shrink_scaled;

        /* First pass: detect and freeze min/max violations without committing the others.
         * The bounds are compared against the EXACT size, scaled by den — a truncated size
         * sits inside a max bound that the real one exceeds, which would leave the item
         * flexible and hand it a size past its bound. */
        bool froze_one = false;
        for (int i = 0; i < n; i++)
        {
            if (items[i].line != ln || items[i].frozen)
                continue;
            const int64_t want = (int64_t)items[i].main * den + flex_share_num(&items[i], remaining, growing);
            int64_t bounded = want;
            if (bounded < 0)
                bounded = 0;
            if (items[i].main_min != ER_LAYOUT_AUTO && bounded < (int64_t)items[i].main_min * den)
                bounded = (int64_t)items[i].main_min * den;
            if (items[i].main_max != ER_LAYOUT_AUTO && bounded > (int64_t)items[i].main_max * den)
                bounded = (int64_t)items[i].main_max * den;
            if (bounded < 0)
                bounded = 0;
            if (bounded != want)
            {
                /* A bound is a whole pixel, so the frozen size divides out exactly. */
                items[i].main = (int16_t)(bounded / den);
                items[i].frozen = 1U;
                froze_one = true;
            }
        }
        if (froze_one)
            continue;

        /* No violations: commit the distributed sizes to every remaining flexible item.
         *
         * Each item spans from one rounded edge to the next, so a leftover that does not
         * divide evenly lands on whichever item straddles the half pixel rather than being
         * dropped at the end of the line. `carry`/`rem` track the running edge exactly; the
         * frozen items in between are whole pixels and never move the fraction. */
        int32_t carry = 0;
        int64_t rem = 0;
        int32_t prev_edge = 0;
        for (int i = 0; i < n; i++)
        {
            if (items[i].line != ln || items[i].frozen)
                continue;
            const int64_t num = flex_share_num(&items[i], remaining, growing);
            int64_t q = num / den;
            int64_t r = num % den; /* Same sign as num; renormalise to a floor division. */
            if (r < 0)
            {
                q--;
                r += den;
            }
            carry += (int32_t)q;
            rem += r;
            if (rem >= den)
            {
                rem -= den;
                carry++;
            }
            const int32_t edge = edge_round(carry, rem, den, rev_main);
            int32_t v = items[i].main + (edge - prev_edge);
            prev_edge = edge;
            if (v < 0)
                v = 0;
            items[i].main = clamp_size((int16_t)v, items[i].main_min, items[i].main_max);
        }
        break;
    }
}

static void measure_content(const uint16_t tag, const int16_t avail_w, int16_t* out_w, int16_t* out_h);

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
 * @param[in]      avail_w   Width the child may occupy, or ER_LAYOUT_AUTO when unconstrained.
 * @param[in,out]  measured  Set to true once this call has measured; skips re-measuring.
 * @param[out]     iw        Receives the intrinsic width (only written on the first call).
 * @param[out]     ih        Receives the intrinsic height (only written on the first call).
 */
static void measure_lazy(const uint16_t tag, const int16_t avail_w, bool* measured, int16_t* iw, int16_t* ih)
{
    if (*measured)
        return;
    measure_content(tag, avail_w, iw, ih);
    *measured = true;
}

/**
 * @brief Width a container offers an auto-width child's measurement, as Yoga's at-most mode does.
 *
 * A child is measured no wider than the container's content box less its own horizontal margins, so
 * the text inside it wraps there: in a column, where the child's width is the cross axis, and in a
 * row, where it is the most the child's flex basis may take. A row that scrolls offers no bound,
 * since its content is meant to overflow it. A percentage width is that fraction of the content box
 * instead, and an explicit width needs no offer at all.
 *
 * measure_content() and Pass 1 both offer children this width, so a container measures its
 * children exactly as it later lays them out.
 *
 * @param[in] L        The container's layout spec.
 * @param[in] cl       The child's layout spec.
 * @param[in] inner_w  The container's content-box width, or ER_LAYOUT_AUTO when unknown.
 *
 * @return The width offered, or ER_LAYOUT_AUTO when the child is measured unconstrained.
 */
static int16_t offered_width(const ERLayoutSpec* L, const ERLayoutSpec* cl, const int16_t inner_w)
{
    if (inner_w == ER_LAYOUT_AUTO)
        return ER_LAYOUT_AUTO;
    if (cl->width_pct > 0.0f)
        return (int16_t)((float)inner_w * cl->width_pct / 100.0f + 0.5f);
    if (cl->width != ER_LAYOUT_AUTO || (is_row_dir(L->flex_direction) && L->overflow == ER_OVERFLOW_SCROLL))
        return ER_LAYOUT_AUTO;
    const int16_t margin_x = (int16_t)(edge_or(cl->margin_left, cl->margin) + edge_or(cl->margin_right, cl->margin));
    return clamp_size((int16_t)(inner_w > margin_x ? inner_w - margin_x : 0), cl->min_width, cl->max_width);
}

/**
 * @brief The main size aspect_ratio gives an item from its definite cross size.
 *
 * @param[in] cl          The item's layout spec.
 * @param[in] is_row      Whether the main axis is horizontal, so the size returned is a width.
 * @param[in] cross_size  Container content-box extent on the cross axis, or ER_LAYOUT_AUTO when unknown.
 *
 * @return The main size, or ER_LAYOUT_AUTO when the item has no aspect_ratio or no definite cross size.
 */
static int16_t aspect_main_size(const ERLayoutSpec* cl, const bool is_row, const int16_t cross_size)
{
    if (cl->aspect_ratio <= 0.0f)
        return ER_LAYOUT_AUTO;
    const float cross_pct = is_row ? cl->height_pct : cl->width_pct;
    int16_t cross = is_row ? cl->height : cl->width;
    if (cross_pct > 0.0f)
    {
        if (cross_size == ER_LAYOUT_AUTO)
            return ER_LAYOUT_AUTO;
        cross = (int16_t)((float)cross_size * cross_pct / 100.0f + 0.5f);
    }
    else if (cross == ER_LAYOUT_AUTO)
        return ER_LAYOUT_AUTO;
    cross = clamp_size(cross, is_row ? cl->min_height : cl->min_width, is_row ? cl->max_height : cl->max_width);
    /* aspect_ratio == width / height. */
    return round_px(is_row ? (float)cross * cl->aspect_ratio : (float)cross / cl->aspect_ratio);
}

/**
 * @brief Whether aspect_ratio gives an item its cross size from its main size.
 *
 * @param[in] cl      The item's layout spec.
 * @param[in] is_row  Whether the container's main axis is horizontal.
 *
 * @return true when the item has an aspect_ratio and no explicit or percentage cross size.
 */
static bool cross_from_aspect(const ERLayoutSpec* cl, const bool is_row)
{
    return cl->aspect_ratio > 0.0f && (is_row ? cl->height : cl->width) == ER_LAYOUT_AUTO
           && (is_row ? cl->height_pct : cl->width_pct) <= 0.0f;
}

/**
 * @brief The cross size aspect_ratio gives an item from its main size.
 *
 * @param[in] cl      The item's layout spec.
 * @param[in] is_row  Whether the main axis is horizontal, so the size returned is a height.
 * @param[in] main    The item's main size.
 *
 * @return The cross size, held to the item's cross-axis min/max.
 */
static int16_t aspect_cross_size(const ERLayoutSpec* cl, const bool is_row, const int16_t main)
{
    const float cross = is_row ? (float)main / cl->aspect_ratio : (float)main * cl->aspect_ratio;
    return clamp_size(
        round_px(cross), is_row ? cl->min_height : cl->min_width, is_row ? cl->max_height : cl->max_width);
}

/**
 * @brief A flex item's base main size — flex_basis_pct (%) > flex_basis (px) > explicit size > aspect ratio >
 * intrinsic.
 *
 * @param[in]      tag         Tag of the item.
 * @param[in]      cl          The item's layout spec.
 * @param[in]      is_row      Whether the container's main axis is horizontal.
 * @param[in]      main_size   Container content-box extent on the main axis; percentages resolve against it.
 * @param[in]      cross_size  Container content-box extent on the cross axis, or ER_LAYOUT_AUTO when unknown.
 * @param[in]      avail_w     Width offered to the item's measurement, or ER_LAYOUT_AUTO when unconstrained.
 * @param[in,out]  measured    Threaded through measure_lazy(), so the measurement is shared with the cross axis.
 * @param[out]     intr_w      Receives the intrinsic width if this call measures.
 * @param[out]     intr_h      Receives the intrinsic height if this call measures.
 *
 * @return The base main size, held to the item's main-axis min/max.
 */
static int16_t base_main_size(const uint16_t tag,
                              const ERLayoutSpec* cl,
                              const bool is_row,
                              const int16_t main_size,
                              const int16_t cross_size,
                              const int16_t avail_w,
                              bool* measured,
                              int16_t* intr_w,
                              int16_t* intr_h)
{
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
        else if ((hypo_main = aspect_main_size(cl, is_row, cross_size)) == ER_LAYOUT_AUTO)
        {
            measure_lazy(tag, avail_w, measured, intr_w, intr_h);
            hypo_main = is_row ? *intr_w : *intr_h;
        }
    }
    return clamp_size(hypo_main, is_row ? cl->min_width : cl->min_height, is_row ? cl->max_width : cl->max_height);
}

/**
 * @brief Re-measures a row item's content height at the width flexing gave it.
 *
 * A row measures its children before it knows how wide they will be, so an item that flexes to
 * another width may wrap its text onto a different number of lines. An item already at the width
 * it was measured at is left alone: measuring again there would give the same answer.
 *
 * @param[in,out] fc  The item; `cross` is updated when its height is its content's.
 */
static void remeasure_cross(FlexChild* fc)
{
    if (fc->measured_w == ER_LAYOUT_AUTO || fc->main == fc->measured_w)
        return;
    int16_t w = 0, h = 0;
    measure_content(fc->tag, fc->main, &w, &h);
    fc->cross = h;
    fc->measured_w = fc->main;
}

/**
 * @brief Looks a measurement up in the per-pass cache.
 *
 * @param[in]  tag      Tag of the node.
 * @param[in]  avail_w  Width the node is offered, or ER_LAYOUT_AUTO.
 * @param[out] out_w    Receives the cached width on a hit.
 * @param[out] out_h    Receives the cached height on a hit.
 *
 * @return true on a hit.
 */
static bool cache_get(const uint16_t tag, const int16_t avail_w, int16_t* out_w, int16_t* out_h)
{
    if (tag >= (uint16_t)ERUI_MAX_NODES)
        return false;
    for (int i = 0; i < MEASURE_CACHE_WAYS; i++)
    {
        const MeasureCacheEntry* e = &s_measure_cache[tag][i];
        if (e->gen == s_measure_gen && e->avail == avail_w)
        {
            *out_w = e->w;
            *out_h = e->h;
            return true;
        }
    }
    return false;
}

/**
 * @brief Stores a measurement in the per-pass cache, evicting the node's oldest one.
 *
 * @param[in] tag      Tag of the node.
 * @param[in] avail_w  Width the node was offered, or ER_LAYOUT_AUTO.
 * @param[in] w        Measured width.
 * @param[in] h        Measured height.
 */
static void cache_put(const uint16_t tag, const int16_t avail_w, const int16_t w, const int16_t h)
{
    if (tag >= (uint16_t)ERUI_MAX_NODES)
        return;
    MeasureCacheEntry* ways = s_measure_cache[tag];
    memmove(&ways[1], &ways[0], sizeof(ways[0]) * (MEASURE_CACHE_WAYS - 1));
    ways[0] = (MeasureCacheEntry){w, h, avail_w, s_measure_gen};
}

/**
 * @brief Returns true when a Text node's content holds an explicit line break.
 *
 * @param[in] tp  The node's text props.
 *
 * @return true when the text, or any of its spans, contains '\n'.
 */
static bool text_has_newline(const ERTextProps* tp)
{
    if (tp->span_count == 0U)
        return strchr(tp->text, '\n') != NULL;
    for (uint8_t i = 0; i < tp->span_count; i++)
        if (strchr(tp->spans[i].text, '\n') != NULL)
            return true;
    return false;
}

/**
 * @brief Measures a Text node's glyphs as er_text_render() will lay them out at a given width.
 *
 * The single-line run is measured first. The text is only broken into lines when it can break:
 * when it is wider than the width it is drawn at, or it holds an explicit newline. A text that fits
 * on one line keeps its single-line measurement; one that breaks takes its widest line as its width,
 * and its line count — at most number_of_lines — times the line height as its height.
 *
 * @param[in]  tp       The node's text props.
 * @param[in]  break_w  Width the glyphs are drawn in, or 0 when nothing bounds it.
 * @param[out] out_w    Receives the content width in pixels.
 * @param[out] out_h    Receives the content height in pixels.
 */
static void measure_text(const ERTextProps* tp, const int break_w, int32_t* out_w, int32_t* out_h)
{
    int measured_w = 0, measured_h = 0;
    if (tp->span_count > 0U)
    {
        /* Styled spans render with per-run weight/spacing — measure them the same way so a
           bold/styled run does not overflow an auto-sized node and clip its trailing glyphs. */
        er_text_measure_spans(tp->spans,
                              tp->span_count,
                              tp->font_size,
                              tp->font_family,
                              tp->letter_spacing,
                              tp->font_weight,
                              &measured_w,
                              &measured_h);
    }
    else
    {
        er_text_measure(
            tp->text, tp->font_size, tp->font_family, tp->letter_spacing, tp->font_weight, &measured_w, &measured_h);
    }
    const int32_t line_h = (tp->line_height > 0) ? tp->line_height : measured_h;

    int lines = 1;
    if ((break_w > 0 && measured_w > break_w) || text_has_newline(tp))
    {
        ERTextRenderParams par;
        memset(&par, 0, sizeof(par));
        par.text = tp->text;
        par.font_size = tp->font_size;
        par.font_family = tp->font_family;
        par.number_of_lines = tp->number_of_lines;
        par.font_weight = tp->font_weight;
        par.letter_spacing = tp->letter_spacing;
        par.span_count = tp->span_count;
        par.spans = (tp->span_count > 0U) ? tp->spans : NULL;
        int broken = 0;
        er_text_measure_lines(&par, break_w, &measured_w, &broken);
        if (broken > 1)
            lines = broken;
    }
    *out_w = measured_w;
    *out_h = line_h * lines;
}

/**
 * @brief Content size of a non-wrapping row whose width is known, flexed the way layout will flex it.
 *
 * A child's height can depend on its width — a Text wraps onto more lines when it is narrower — so a
 * row that knows its width resolves its children's flex_grow / flex_shrink first, exactly as Pass 3
 * will, and measures each one's height at the width it is given. The row's width is then what its
 * children take up once flexed.
 *
 * The items live at the top of s_scratch (see s_measure_top), below any a measurement further up the
 * tree has claimed, and are released before returning.
 *
 * @param[in]  n         The row.
 * @param[in]  inner_w   The row's content-box width.
 * @param[in]  inner_h   The row's content-box height, or ER_LAYOUT_AUTO when unknown.
 * @param[in]  main_gap  Gap between adjacent children.
 * @param[out] out_w     Receives the content width.
 * @param[out] out_h     Receives the content height.
 *
 * @return false when the row has no in-flow children or s_scratch has no room; nothing is written then.
 */
static bool measure_flexed_row(const ERNode* n,
                               const int16_t inner_w,
                               const int16_t inner_h,
                               const int16_t main_gap,
                               int16_t* out_w,
                               int16_t* out_h)
{
    int count = 0;
    for (uint16_t ct = n->first_child_tag; ct != ER_INVALID_TAG;)
    {
        const ERNode* c = er_get_node(ct);
        if (!c)
            break;
        if (c->layout.position != ER_POS_ABSOLUTE && c->layout.display != ER_DISPLAY_NONE)
            count++;
        ct = c->next_sibling_tag;
    }
    if (count == 0 || count > s_measure_top)
        return false;

    const int top = s_measure_top;
    s_measure_top -= count;
    FlexChild* items = &s_scratch[s_measure_top];

    int i = 0;
    for (uint16_t ct = n->first_child_tag; ct != ER_INVALID_TAG && i < count;)
    {
        const ERNode* c = er_get_node(ct);
        if (!c)
            break;
        if (c->layout.position != ER_POS_ABSOLUTE && c->layout.display != ER_DISPLAY_NONE)
        {
            const ERLayoutSpec* cl = &c->layout;
            const int16_t avail_w = offered_width(&n->layout, cl, inner_w);
            bool measured = false;
            int16_t intr_w = 0, intr_h = 0;

            FlexChild* fc = &items[i++];
            memset(fc, 0, sizeof(*fc));
            fc->tag = ct;
            fc->main = base_main_size(ct, cl, true, inner_w, inner_h, avail_w, &measured, &intr_w, &intr_h);
            measure_lazy(ct, avail_w, &measured, &intr_w, &intr_h);
            fc->cross = intr_h;
            fc->measured_w = (cl->height == ER_LAYOUT_AUTO) ? intr_w : ER_LAYOUT_AUTO;
            fc->margin_main_start = edge_or(cl->margin_left, cl->margin);
            fc->margin_main_end = edge_or(cl->margin_right, cl->margin);
            fc->margin_cross_start = edge_or(cl->margin_top, cl->margin);
            fc->margin_cross_end = edge_or(cl->margin_bottom, cl->margin);
            fc->flex_grow = cl->flex_grow;
            fc->flex_shrink = cl->flex_shrink;
            fc->main_min = cl->min_width;
            fc->main_max = cl->max_width;
        }
        ct = c->next_sibling_tag;
    }

    /* A scroll container's children keep their own sizes, as in Pass 3. */
    if (n->layout.overflow != ER_OVERFLOW_SCROLL)
        resolve_flex_line(items, i, 0, inner_w, main_gap, is_reverse_dir(n->layout.flex_direction));

    int32_t main_sum = (i > 1) ? (int32_t)main_gap * (i - 1) : 0;
    int16_t cross_max = 0;
    for (int k = 0; k < i; k++)
    {
        const ERNode* c = er_get_node(items[k].tag);
        if (c && cross_from_aspect(&c->layout, true))
            items[k].cross = aspect_cross_size(&c->layout, true, items[k].main);
        else
            remeasure_cross(&items[k]);
        main_sum += (int32_t)items[k].main + items[k].margin_main_start + items[k].margin_main_end;
        const int32_t outer_h = (int32_t)items[k].cross + items[k].margin_cross_start + items[k].margin_cross_end;
        if (outer_h > cross_max)
            cross_max = (int16_t)(outer_h > INT16_MAX ? INT16_MAX : outer_h);
    }

    s_measure_top = top;
    *out_w = (int16_t)(main_sum > INT16_MAX ? INT16_MAX : (main_sum < 0 ? 0 : main_sum));
    *out_h = cross_max;
    return true;
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
 * Without a known width this is max-content sizing and flex grow/shrink is ignored; the parent
 * applies flex distribution later against the available space.
 *
 * When the width is known — an explicit width, or `avail_w` from the parent — a Text breaks into
 * the lines er_text_render() will draw at that width, and a wrapping row breaks its children into
 * lines and reports the summed line heights, as Yoga does, so an auto-height node grows to hold
 * every line. Each child is offered the width offered_width() gives it, at most the content box. A
 * non-wrapping row also flexes its children at that width and measures their heights at the widths
 * they get (measure_flexed_row()).
 *
 * The walk never touches compute_layout's half of s_scratch, or s_line_cross — a flexed row takes
 * its items from the top of s_scratch — so it is safe to call from within compute_layout's Pass 1
 * without disturbing the in-progress layout state.
 *
 * @param[in]  tag      Tag of the node to measure.
 * @param[in]  avail_w  Width the node may occupy, or ER_LAYOUT_AUTO when unconstrained.
 * @param[out] out_w  Receives the intrinsic width in pixels.
 * @param[out] out_h  Receives the intrinsic height in pixels.
 */
static void measure_content(const uint16_t tag, const int16_t avail_w, int16_t* out_w, int16_t* out_h)
{
    /* Per-pass memoisation: measure_content(tag, avail_w) is a pure function of that node's own
     * subtree for the duration of one er_layout_compute() call, so a cache hit here is
     * exactly what turns the O(depth) remeasurement into O(1) per node per pass. */
    if (cache_get(tag, avail_w, out_w, out_h))
        return;

    ERNode* n = er_get_node(tag);
    if (!n)
    {
        *out_w = 0;
        *out_h = 0;
        return;
    }

    const ERLayoutSpec* L = &n->layout;
    int16_t exp_w = L->width;
    int16_t exp_h = L->height;
    /* aspect_ratio turns a definite size on one axis into one on the other. A percentage width
     * arrives already resolved as avail_w. */
    if (L->aspect_ratio > 0.0f)
    {
        if (exp_w == ER_LAYOUT_AUTO && L->width_pct > 0.0f)
            exp_w = avail_w;
        if (exp_w == ER_LAYOUT_AUTO)
            exp_w = aspect_main_size(L, true, ER_LAYOUT_AUTO);
        else if (exp_h == ER_LAYOUT_AUTO)
            exp_h = aspect_cross_size(L, true, clamp_size(exp_w, L->min_width, L->max_width));
    }

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
            /* The glyphs are drawn in the node's content box: its own width, else the width it is
             * offered, held to its min/max. Nothing is drawn in a box with no width, so a box no
             * wider than its padding is treated as unbounded rather than as one glyph per line. */
            int16_t box_w = exp_w;
            if (box_w == ER_LAYOUT_AUTO)
                box_w = (avail_w != ER_LAYOUT_AUTO) ? clamp_size(avail_w, L->min_width, L->max_width) : L->max_width;
            const int break_w =
                (box_w != ER_LAYOUT_AUTO && box_w > tp.left + tp.right) ? box_w - tp.left - tp.right : 0;

            int32_t text_w = 0, text_h = 0;
            measure_text(&n->props.text, break_w, &text_w, &text_h);
            text_w += tp.left + tp.right;
            text_h += tp.top + tp.bottom;
            tw = (exp_w != ER_LAYOUT_AUTO) ? exp_w : (int16_t)(text_w > INT16_MAX ? INT16_MAX : text_w);
            th = (exp_h != ER_LAYOUT_AUTO) ? exp_h : (int16_t)(text_h > INT16_MAX ? INT16_MAX : text_h);
        }
        *out_w = clamp_size(tw, L->min_width, L->max_width);
        *out_h = clamp_size(th, L->min_height, L->max_height);
        cache_put(tag, avail_w, *out_w, *out_h);
        return;
    }

    /* Container / leaf View: derive content size from in-flow children (only when an axis is auto). */
    const ERPadding pad = er_layout_padding(L);
    int16_t content_w = 0, content_h = 0;
    if (n->first_child_tag != ER_INVALID_TAG && (exp_w == ER_LAYOUT_AUTO || exp_h == ER_LAYOUT_AUTO))
    {
        const bool is_row = is_row_dir(L->flex_direction);
        const int16_t main_gap = is_row ? edge_or(L->column_gap, L->gap) : edge_or(L->row_gap, L->gap);
        const int16_t cross_gap = is_row ? edge_or(L->row_gap, L->gap) : edge_or(L->column_gap, L->gap);
        /* A node's own min/max width bounds what it is offered, so lines break where layout breaks them. */
        const int16_t offered_w =
            (avail_w == ER_LAYOUT_AUTO) ? ER_LAYOUT_AUTO : clamp_size(avail_w, L->min_width, L->max_width);
        const int16_t box_w = (exp_w != ER_LAYOUT_AUTO) ? exp_w : offered_w;
        const int16_t inner_w = (box_w == ER_LAYOUT_AUTO)
                                    ? ER_LAYOUT_AUTO
                                    : (int16_t)(box_w > pad.left + pad.right ? box_w - pad.left - pad.right : 0);
        /* A wrapping row with a known width breaks into lines; the main axis is then the widest
         * line and the cross axis the summed line heights plus the gaps between them. */
        const bool wraps = is_row && L->flex_wrap != ER_WRAP_NOWRAP && inner_w != ER_LAYOUT_AUTO;
        const int16_t inner_h = (exp_h == ER_LAYOUT_AUTO)
                                    ? ER_LAYOUT_AUTO
                                    : (int16_t)(exp_h > pad.top + pad.bottom ? exp_h - pad.top - pad.bottom : 0);
        /* A row that knows its width flexes its children first: their heights depend on the widths they get. */
        const bool flexed = is_row && !wraps && inner_w != ER_LAYOUT_AUTO
                            && measure_flexed_row(n, inner_w, inner_h, main_gap, &content_w, &content_h);
        if (!flexed)
        {
            int32_t main_sum = 0;
            int16_t cross_max = 0;
            int32_t wrapped_cross = 0;
            int32_t widest_line = 0;
            int count = 0;
            int line_count = 0;
            for (uint16_t ct = n->first_child_tag; ct != ER_INVALID_TAG;)
            {
                ERNode* c = er_get_node(ct);
                if (!c)
                    break;
                if (c->layout.position != ER_POS_ABSOLUTE && c->layout.display != ER_DISPLAY_NONE)
                {
                    int16_t cw = 0, ch = 0;
                    const ERLayoutSpec* cl = &c->layout;
                    const int16_t margin_x =
                        (int16_t)(edge_or(cl->margin_left, cl->margin) + edge_or(cl->margin_right, cl->margin));
                    measure_content(ct, offered_width(L, cl, inner_w), &cw, &ch);
                    const int16_t outer_w = (int16_t)(cw + margin_x);
                    const int16_t outer_h =
                        (int16_t)(ch + edge_or(cl->margin_top, cl->margin) + edge_or(cl->margin_bottom, cl->margin));
                    if (wraps)
                    {
                        if (line_count > 0 && main_sum + main_gap + outer_w > inner_w)
                        {
                            wrapped_cross += cross_max + cross_gap;
                            if (main_sum > widest_line)
                                widest_line = main_sum;
                            main_sum = 0;
                            cross_max = 0;
                            line_count = 0;
                        }
                        main_sum += (line_count > 0 ? main_gap : 0) + outer_w;
                        if (outer_h > cross_max)
                            cross_max = outer_h;
                        line_count++;
                    }
                    else if (is_row)
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
            if (wraps)
            {
                if (widest_line > main_sum)
                    main_sum = widest_line;
                cross_max = (int16_t)(wrapped_cross + cross_max > INT16_MAX ? INT16_MAX : wrapped_cross + cross_max);
            }
            else if (count > 1)
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
    }

    const int16_t iw = (exp_w != ER_LAYOUT_AUTO) ? exp_w : (int16_t)(content_w + pad.left + pad.right);
    const int16_t ih = (exp_h != ER_LAYOUT_AUTO) ? exp_h : (int16_t)(content_h + pad.top + pad.bottom);
    *out_w = clamp_size(iw, L->min_width, L->max_width);
    *out_h = clamp_size(ih, L->min_height, L->max_height);
    cache_put(tag, avail_w, *out_w, *out_h);
}

/**
 * @brief Recursively computes layout for the subtree rooted at tag.
 *
 * Implements a 7-pass Yoga-compatible algorithm:
 *   Pass 1 — collect in-flow children; determine hypothetical main / cross sizes.
 *   Pass 2 — assign children to wrap lines.
 *   Pass 3 — resolve flex_grow / flex_shrink against each line's free space; in a row, re-measure
 *            the content heights of the children it resized.
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
    const bool rev_main = is_reverse_dir(L->flex_direction);
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

            /* Width the child may occupy, offered to its measurement so text and wrapping rows
             * inside it break into lines at the content box's edge. */
            const int16_t avail_w = offered_width(L, cl, content_w);

            int16_t hypo_main =
                base_main_size(ct, cl, is_row, main_size, cross_avail, avail_w, &measured, &intr_w, &intr_h);
            const int16_t main_mn = is_row ? cl->min_width : cl->min_height;
            const int16_t main_mx = is_row ? cl->max_width : cl->max_height;

            /* Base cross size (stretch may override later if still auto). When aspect_ratio
             * will drive the cross size after flexing, skip measuring. */
            const int16_t crosssz = is_row ? cl->height : cl->width;
            const float cross_pct = is_row ? cl->height_pct : cl->width_pct;
            const int16_t cross_mn = is_row ? cl->min_height : cl->min_width;
            const int16_t cross_mx = is_row ? cl->max_height : cl->max_width;
            const bool cross_via_aspect = cross_from_aspect(cl, is_row);
            int16_t hypo_cross;
            int16_t measured_w = ER_LAYOUT_AUTO;
            if (cross_pct > 0.0f)
                hypo_cross = (int16_t)((float)cross_avail * cross_pct / 100.0f + 0.5f);
            else if (crosssz != ER_LAYOUT_AUTO)
                hypo_cross = crosssz;
            else if (cross_via_aspect)
                hypo_cross = 0; /* placeholder — set from the flexed main size after Pass 3 */
            else
            {
                measure_lazy(ct, avail_w, &measured, &intr_w, &intr_h);
                hypo_cross = is_row ? intr_h : intr_w;
                if (is_row)
                    measured_w = intr_w;
            }
            hypo_cross = clamp_size(hypo_cross, cross_mn, cross_mx);

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
            fc->measured_w = measured_w;
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
            resolve_flex_line(s_scratch, n_inflow, ln, main_size, main_gap, rev_main);
    }

    /* aspect_ratio gives the cross size from the main size each item flexed to. */
    for (int i = 0; i < n_inflow; i++)
    {
        const ERNode* c = er_get_node(s_scratch[i].tag);
        if (c && cross_from_aspect(&c->layout, is_row))
            s_scratch[i].cross = aspect_cross_size(&c->layout, is_row, s_scratch[i].main);
    }

    /*--------------------------------------------------------------------------
     * Pass 3b — in a row, re-measure each child whose height is its content's at the width it
     * flexed to: a Text given less room than its single line wraps onto more lines, and a line is as
     * tall as its tallest child. A stretched child of a single-line row takes the row's height
     * whatever it measures, so it is skipped.
     *------------------------------------------------------------------------*/
    if (is_row)
    {
        for (int i = 0; i < n_inflow; i++)
        {
            if (L->flex_wrap == ER_WRAP_NOWRAP && s_scratch[i].align == ER_ALIGN_STRETCH)
                continue;
            remeasure_cross(&s_scratch[i]);
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

    /*--------------------------------------------------------------------------
     * Pass 5 — main-axis positions (justifyContent) + cross-axis (alignSelf).
     *------------------------------------------------------------------------*/
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
        /* A stretched line's SIZE comes off the pixel grid the way a flex item's does — the
         * difference of its two rounded cross edges — so the leftover lands on the line that
         * straddles the half pixel instead of being dropped at the far edge. What that rounding left
         * over rides along in the numerators below, so what sits INSIDE the line is still placed
         * exactly. wrap-reverse measures the edges from the far side, so it mirrors them first. */
        const int32_t grow_lo = rev_cross ? -(int32_t)(ln + 1) * ac_stretch : (int32_t)ln * ac_stretch;
        const int32_t grow_hi = rev_cross ? -(int32_t)ln * ac_stretch : (int32_t)(ln + 1) * ac_stretch;
        const int16_t grow_px = (int16_t)(div_round(grow_hi, ac_den) - div_round(grow_lo, ac_den));
        const int32_t grow_rem = ac_stretch - (int32_t)grow_px * ac_den;

        const int16_t this_cross = (int16_t)(s_line_cross[ln] + grow_px);
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
                    /* Stretch fills cross-axis only when size was auto (no explicit px or %, and no
                     * aspect_ratio giving it, as in Yoga). */
                    const ERNode* c = er_get_node(s_scratch[i].tag);
                    if (c)
                    {
                        const int16_t cz = is_row ? c->layout.height : c->layout.width;
                        const float cz_pct = is_row ? c->layout.height_pct : c->layout.width_pct;
                        if (cz == ER_LAYOUT_AUTO && cz_pct <= 0.0f && c->layout.aspect_ratio <= 0.0f)
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
                    num = (int32_t)(inner - s_scratch[i].cross) * ac_den + grow_rem;
                    break;
                case ER_ALIGN_FLEX_END:
                    pos = this_cross - s_scratch[i].margin_cross_end - s_scratch[i].cross;
                    num = 2 * grow_rem;
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

        /* Relative-position offsets (left/right/top/bottom shift in flow). A percentage measures against
         * this parent's CONTENT box, not the padding box an absolute's insets resolve against. */
        const ERLayoutSpec* cl = &c->layout;
        float d = 0.0f;
        if (inset_px(cl->left, cl->left_pct, content_w, &d))
            cx = round_px((float)cx + d);
        else if (inset_px(cl->right, cl->right_pct, content_w, &d))
            cx = round_px((float)cx - d);
        if (inset_px(cl->top, cl->top_pct, content_h, &d))
            cy = round_px((float)cy + d);
        else if (inset_px(cl->bottom, cl->bottom_pct, content_h, &d))
            cy = round_px((float)cy - d);

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
             * answer is left unresolved for the aspect-ratio and content fallbacks below.
             *
             * Insets and sizes stay FRACTIONAL through all of this. A percentage rarely lands on a whole
             * pixel, and rounding one before it is combined with the rest would round twice. */
            float il = 0.0f, ir = 0.0f, it = 0.0f, ib = 0.0f;
            const bool has_l = inset_px(cl->left, cl->left_pct, w, &il);
            const bool has_r = inset_px(cl->right, cl->right_pct, w, &ir);
            const bool has_t = inset_px(cl->top, cl->top_pct, h, &it);
            const bool has_b = inset_px(cl->bottom, cl->bottom_pct, h, &ib);

            float cw = 0.0f, ach = 0.0f;
            bool have_w = true, have_h = true;
            if (cl->width != ER_LAYOUT_AUTO)
                cw = (float)cl->width;
            else if (cl->width_pct > 0.0f)
                cw = (float)w * cl->width_pct / 100.0f;
            else if (has_l && has_r)
                cw = (float)(w - ml - mr) - il - ir;
            else
                have_w = false;

            if (cl->height != ER_LAYOUT_AUTO)
                ach = (float)cl->height;
            else if (cl->height_pct > 0.0f)
                ach = (float)h * cl->height_pct / 100.0f;
            else if (has_t && has_b)
                ach = (float)(h - mt - mb) - it - ib;
            else
                have_h = false;

            /* aspect_ratio (= width / height) fills an axis from the one already resolved, exactly as
             * Pass 1 derives a flow child's cross size from its main. */
            if (cl->aspect_ratio > 0.0f && have_w != have_h)
            {
                if (have_w)
                {
                    ach = cw / cl->aspect_ratio;
                    have_h = true;
                }
                else
                {
                    cw = ach * cl->aspect_ratio;
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
                measure_content(ct, have_w ? (int16_t)(cw + 0.5f) : ER_LAYOUT_AUTO, &intr_w, &intr_h);
                if (!have_w)
                    cw = (float)intr_w;
                if (!have_h)
                    ach = (float)intr_h;
            }

            cw = clamp_sizef(cw, cl->min_width, cl->max_width);
            ach = clamp_sizef(ach, cl->min_height, cl->max_height);

            /* An axis with NO inset falls back to its static position: where this child would have sat
             * had it stayed in flow, which is inside the CONTENT box and honours the parent's
             * justifyContent / alignItems (the child's own alignSelf winning, as in Pass 1). Resolved
             * per axis, so `left: 10` with no `top` pins x and still aligns y. */
            uint8_t self_align = (cl->align_self != ER_ALIGN_AUTO) ? cl->align_self : L->align_items;
            if (self_align == ER_ALIGN_AUTO)
                self_align = ER_ALIGN_STRETCH;

            /* The static position places a whole node, so it works off the rounded size. */
            const int16_t rw = round_px(cw);
            const int16_t rh = round_px(ach);
            const int16_t s_main = is_row ? rw : rh;
            const int16_t s_cross = is_row ? rh : rw;
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
            float ex, ey;
            if (has_l)
                ex = (float)(x + ml) + il;
            else if (has_r)
                ex = (float)(x + w - mr) - ir - cw;
            else
                ex = (float)(is_row ? main_static : cross_static);

            if (has_t)
                ey = (float)(y + mt) + it;
            else if (has_b)
                ey = (float)(y + h - mb) - ib - ach;
            else
                ey = (float)(is_row ? cross_static : main_static);

            /* Both edges land on the pixel grid and the size is the distance between them, so a box
             * straddling a half pixel keeps it instead of losing it to a rounded-down size. */
            const int16_t cx = round_px(ex);
            const int16_t cy = round_px(ey);
            compute_layout(ct, span_px(cx, round_px(ex + cw)), span_px(cy, round_px(ey + ach)), cx, cy);
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
