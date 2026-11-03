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

/** @brief A percentage field divided by this is a fraction of its base. */
#define PERCENT_SCALE 100.0f

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
    int16_t measured_width; /**< Row item sized by its content's height: the width measured at (else AUTO). */
    uint8_t line;       /**< Wrap-line index. */
    uint8_t align;      /**< Resolved align (auto → parent align_items). */
    uint8_t frozen;     /**< Pass 3: 1 once the item's flexed main size is final. */
} FlexChild;

/**
 * @brief Per-pass memoisation slots for measure_content(), keyed by node tag.
 *
 * measure_content() depends only on that node's own subtree, which nothing mutates during
 * a layout pass, and on the width it is offered — so for a given (tag, available_width) pair its result
 * is a constant for the whole er_layout_compute() call. Without this cache, compute_layout()
 * re-measures every child once per Pass 1 it participates in *and* recurses into that
 * same child, so a leaf at ancestor depth d gets remeasured d times. The `generation` tags make
 * the cache self-invalidating across passes without a memset: a slot is only valid when
 * its generation matches the current pass's s_measure_generation.
 *
 * A row measures a child twice — at most the row's width for its flex basis, then at the width
 * flexing gives it — so each node keeps its two most recent measurements (MEASURE_CACHE_WAYS).
 * With one, the second would evict the first, and the row's own layout pass, which asks for both
 * again, would measure the child's whole subtree twice more.
 */
typedef struct
{
    int16_t width, height;
    int16_t available_width; /**< Width the cached size was measured against (ER_LAYOUT_AUTO = unconstrained). */
    uint16_t generation;
} MeasureCacheEntry;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static FlexChild s_scratch[ERUI_MAX_NODES];
static int16_t s_line_cross[ERUI_MAX_NODES];
/** @brief Measurements kept per node; way 0 is the most recent. */
#define MEASURE_CACHE_WAYS 2

static MeasureCacheEntry s_measure_cache[ERUI_MAX_NODES][MEASURE_CACHE_WAYS];
static uint16_t s_measure_generation; /**< Current layout pass id; 0 means "no pass has run yet". */

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
 * @brief Clamps value to [minimum, maximum], treating ER_LAYOUT_AUTO bounds as unconstrained.
 *
 * @param[in] value    Value to clamp.
 * @param[in] minimum  Minimum bound (ER_LAYOUT_AUTO = no minimum).
 * @param[in] maximum  Maximum bound (ER_LAYOUT_AUTO = no maximum).
 *
 * @return The clamped value, never negative.
 */
static int16_t clamp_size(int16_t value, const int16_t minimum, const int16_t maximum)
{
    if (minimum != ER_LAYOUT_AUTO && value < minimum)
        value = minimum;
    if (maximum != ER_LAYOUT_AUTO && value > maximum)
        value = maximum;
    if (value < 0)
        value = 0;
    return value;
}

/**
 * @brief Clamps a fractional size to [minimum, maximum], treating ER_LAYOUT_AUTO bounds as unconstrained.
 *
 * The float twin of clamp_size(), for the one place a size is still fractional: an absolute's size can
 * be a percentage, or the gap left between two percentage insets, and it is not rounded until both of
 * its edges are — clamping it as an integer here would round it a second time.
 *
 * @param[in] value    Value to clamp.
 * @param[in] minimum  Minimum bound (ER_LAYOUT_AUTO = no minimum).
 * @param[in] maximum  Maximum bound (ER_LAYOUT_AUTO = no maximum).
 *
 * @return The clamped value, never negative.
 */
static float clamp_sizef(float value, const int16_t minimum, const int16_t maximum)
{
    /* Settled first, because every comparison below is false for a NaN — it would sail through the
     * clamps untouched and break the contract this function exists to enforce. */
    if (value != value)
        value = 0.0f;
    if (minimum != ER_LAYOUT_AUTO && value < (float)minimum)
        value = (float)minimum;
    if (maximum != ER_LAYOUT_AUTO && value > (float)maximum)
        value = (float)maximum;
    if (value < 0.0f)
        value = 0.0f;
    return value;
}

/**
 * @brief Rounds a fractional pixel coordinate half up, the way div_round() rounds an exact fraction.
 *
 * `(int)(coordinate + 0.5f)` truncates toward zero, which rounds a negative coordinate the wrong way — and a
 * coordinate here is routinely negative, since an inset may be negative and an absolute is free to sit
 * outside its containing block.
 *
 * @param[in] coordinate  Coordinate in pixels.
 *
 * @return floor(coordinate + 1/2).
 */
static int16_t round_px(const float coordinate)
{
    /* Float-to-int is undefined past int16 and for a NaN, and both are reachable from a style: a
     * percentage is spelled as text, so `'NaN%'` parses and `'100000%'` of a full screen does not fit.
     * Settle them here rather than letting a coordinate wrap into the opposite corner. */
    if (coordinate != coordinate)
        return 0;
    if (coordinate >= (float)INT16_MAX)
        return INT16_MAX;
    if (coordinate <= (float)INT16_MIN)
        return INT16_MIN;

    const float shifted = coordinate + 0.5f;
    int32_t truncated = (int32_t)shifted;
    if ((float)truncated > shifted)
        truncated--;
    return (int16_t)truncated;
}

/**
 * @brief The whole-pixel distance between two rounded edges — a size on the pixel grid.
 *
 * round_px() clamps each edge into int16 separately, so a wild percentage can saturate one edge to each
 * end of that range and leave a difference twice as wide as an int16, or a reversed one. A size is
 * never negative and never leaves int16, so it is settled here rather than wrapping on the cast.
 *
 * @param[in] near_edge  Rounded near edge.
 * @param[in] far_edge   Rounded far edge.
 *
 * @return hi - lo, clamped to [0, INT16_MAX].
 */
static int16_t span_px(const int16_t near_edge, const int16_t far_edge)
{
    const int32_t distance = (int32_t)far_edge - (int32_t)near_edge;
    if (distance < 0)
        return 0;
    if (distance > INT16_MAX)
        return INT16_MAX;
    return (int16_t)distance;
}

/**
 * @brief Resolves one inset edge — left, top, right or bottom — to pixels.
 *
 * A percentage measures against `base`, the containing block's extent on the edge's OWN axis: left and
 * right take its width, top and bottom its height. It wins over the pixel field the way width_pct wins
 * over width, and may be negative. `0.0` is the "not set" sentinel, so the bindings lower an authored
 * `0%` onto the PIXEL field instead — 0% of any box is 0 pixels, so it pins the edge either way.
 * A NaN percentage is not a value and reads as unset.
 *
 * The result stays fractional so the caller can round the edge it lands on exactly once.
 *
 * @param[in]  pixels   Pixel field (ER_LAYOUT_AUTO = not set).
 * @param[in]  percent  Percentage field (0.0 = not set).
 * @param[in]  base     Containing-block extent the percentage measures against.
 * @param[out] out      Receives the resolved inset; untouched when the edge is not set.
 *
 * @return true when the edge is set.
 */
static bool inset_px(const int16_t pixels, const float percent, const int16_t base, float* out)
{
    /* `percent == percent` is the NaN test: a percentage is spelled as text, so `'NaN%'` parses. A NaN is not a
     * value, so the edge falls back to the pixel field rather than poisoning the whole axis — a NaN
     * position would take the node's size down with it. */
    if (percent != 0.0f && percent == percent)
    {
        *out = (float)base * percent / PERCENT_SCALE;
        return true;
    }
    if (pixels == ER_LAYOUT_AUTO)
        return false;
    *out = (float)pixels;
    return true;
}

/**
 * @brief Divides and rounds a half up, matching Yoga's pixel grid.
 *
 * Layout positions round the way Yoga does: a coordinate on a half pixel goes up. Plain C division
 * truncates instead, which lands a pixel short.
 *
 * Round a position ONCE, after everything that shifts it — including the mirror a reversed axis
 * applies. `numerator` may carry the whole-pixel part of the position or only the fraction; Pass 5 leaves
 * it out just to keep the operands small.
 *
 * @param[in] numerator    Numerator. May be negative: an item wider than the box it is centred in overhangs.
 * @param[in] denominator  Denominator. Must be > 0.
 *
 * @return floor(numerator / denominator + 1/2).
 */
static int32_t div_round(const int32_t numerator, const int32_t denominator)
{
    int32_t quotient = numerator / denominator;
    int32_t remainder = numerator % denominator; /* Same sign as numerator (C99 6.5.5p6). */

    /* Renormalise to a floor quotient with a remainder in [0, denominator), so the half-up test below reads
     * the same for negative numerators as for positive ones. */
    if (remainder < 0)
    {
        quotient--;
        remainder += denominator;
    }
    if (2 * remainder >= denominator)
        quotient++;
    return quotient;
}

/**
 * @brief Rounds a cumulative edge held as `carry + remainder / denominator` to a whole pixel.
 *
 * Sizes ride the same pixel grid as positions: an item spans from one rounded edge to the next, so a
 * leftover that does not divide evenly lands on whichever item straddles the half pixel instead of
 * being dropped at the end. Splitting the edge into a whole part and a fraction keeps the running sum
 * small enough to stay exact no matter how many items share the line.
 *
 * @param[in] carry        Whole pixels of the edge.
 * @param[in] remainder    Fraction of a pixel still outstanding, in [0, denominator).
 * @param[in] denominator  Denominator of the fraction. Must be > 0.
 * @param[in] reversed     Whether the axis measures this edge from its far end.
 *
 * @return The edge rounded to a whole pixel.
 */
static int32_t edge_round(const int32_t carry, const int64_t remainder, const int64_t denominator, const bool reversed)
{
    /* A reversed axis mirrors the edge before rounding it, and mirroring flips which way a value
     * sitting exactly on a half pixel goes: round(C - v) is C - v rounded DOWN there. */
    const int64_t twice = 2 * remainder;
    return carry + ((reversed ? (twice > denominator) : (twice >= denominator)) ? 1 : 0);
}

/**
 * @brief One flex item's exact share of a line's free space, as a numerator over a shared denominator.
 *
 * The denominator is the line's `total_grow` when growing and its `total_shrink_scaled` when
 * shrinking, so every item on the line is measured against the same scale and their shares add up
 * exactly. Yoga carries this as a float all the way to the end; keeping it as a fraction is how the
 * integer engine does the same.
 *
 * @param[in] item      The item.
 * @param[in] remaining  Free main-axis space on the line: positive when growing, negative when shrinking.
 * @param[in] growing    Whether the line is growing (flex_grow) rather than shrinking (flex_shrink).
 *
 * @return The share's numerator. Negative when shrinking.
 */
static int64_t flex_share_num(const FlexChild* item, const int32_t remaining, const bool growing)
{
    if (growing)
        return (int64_t)remaining * item->flex_grow;
    return (int64_t)remaining * ((int64_t)item->flex_shrink * item->main);
}

/**
 * @brief Returns true when direction is a row-based flex direction.
 *
 * @param[in] direction  ERFlexDirection value.
 *
 * @return true for ER_FLEX_ROW or ER_FLEX_ROW_REVERSE, false otherwise.
 */
static bool is_row_dir(const uint8_t direction)
{
    return direction == ER_FLEX_ROW || direction == ER_FLEX_ROW_REVERSE;
}

/**
 * @brief Returns true when direction is a reversed flex direction.
 *
 * @param[in] direction  ERFlexDirection value.
 *
 * @return true for ER_FLEX_ROW_REVERSE or ER_FLEX_COL_REVERSE, false otherwise.
 */
static bool is_reverse_dir(const uint8_t direction)
{
    return direction == ER_FLEX_ROW_REVERSE || direction == ER_FLEX_COL_REVERSE;
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
 * @param[in] start           Screen coordinate of the content box on this axis.
 * @param[in] available       Content-box extent on this axis.
 * @param[in] size            The item's resolved size on this axis.
 * @param[in] margin_lead     Margin on the axis-start side.
 * @param[in] margin_trail    Margin on the axis-end side.
 * @param[in] doubled_offset  TWICE the offset, from solo_main_offset2() / solo_cross_offset2().
 * @param[in] reversed        Whether this axis runs from the far edge.
 *
 * @return The item's screen origin on this axis.
 */
static int16_t solo_axis_pos(const int16_t start,
                             const int16_t available,
                             const int16_t size,
                             const int16_t margin_lead,
                             const int16_t margin_trail,
                             const int32_t doubled_offset,
                             const bool reversed)
{
    /* Stay in half pixels until the mirror is done, then round once. */
    const int32_t doubled_position = reversed ? (2 * (int32_t)(available - margin_trail - size) - doubled_offset)
                                              : (doubled_offset + 2 * (int32_t)margin_lead);
    return (int16_t)(start + div_round(doubled_position, 2));
}

/**
 * @brief Resolves flex_grow / flex_shrink for the items of one line against its free space.
 *
 * Yoga's resolve-flexible-lengths loop: distribute the remaining free space over the unfrozen
 * items; any that hit a min/max bound freeze at the clamped size and their freed space is
 * redistributed to the rest on the next round. Shared by layout (Pass 3) and by measure_content(),
 * which has to flex a row to learn how tall its children are at the widths they end up with.
 *
 * @param[in,out] items          Flex items; `main` holds each base size on entry and the flexed size on return.
 * @param[in]     item_count     Number of entries in items.
 * @param[in]     line_index     Line to resolve; items on other lines are left alone.
 * @param[in]     main_size      Content-box extent of the container on the main axis.
 * @param[in]     main_gap       Gap between adjacent items on the main axis.
 * @param[in]     reversed_main  Whether the main axis is reversed (it decides which way a half pixel rounds).
 */
static void resolve_flex_line(FlexChild* items,
                              const int item_count,
                              const int line_index,
                              const int16_t main_size,
                              const int16_t main_gap,
                              const bool reversed_main)
{
    /* Free space against the children's hypothetical (base) main sizes. */
    int32_t base_used = 0;
    int count = 0;
    for (int item_index = 0; item_index < item_count; item_index++)
    {
        if (items[item_index].line != line_index)
            continue;
        base_used += items[item_index].main + items[item_index].margin_main_start + items[item_index].margin_main_end;
        count++;
    }
    if (count > 1)
        base_used += (count - 1) * main_gap;
    const int32_t initial_free = (int32_t)main_size - base_used;
    if (initial_free == 0)
        return;

    /* Positive free space grows (flex_grow); negative shrinks (flex_shrink). Items with
     * no factor in the active direction are frozen at their base size from the start. */
    const bool growing = initial_free > 0;
    for (int item_index = 0; item_index < item_count; item_index++)
    {
        if (items[item_index].line != line_index)
            continue;
        items[item_index].frozen =
            (uint8_t)(growing ? (items[item_index].flex_grow == 0) : (items[item_index].flex_shrink == 0));
    }

    /* Each round freezes at least one item or commits and exits, so it runs at most `count` times. */
    for (int guard = 0; guard <= count; guard++)
    {
        int32_t used = 0;
        int32_t total_grow = 0;
        int64_t total_shrink_scaled = 0;
        int unfrozen = 0;
        for (int item_index = 0; item_index < item_count; item_index++)
        {
            if (items[item_index].line != line_index)
                continue;
            used += items[item_index].main + items[item_index].margin_main_start + items[item_index].margin_main_end;
            if (!items[item_index].frozen)
            {
                total_grow += items[item_index].flex_grow;
                total_shrink_scaled += (int64_t)items[item_index].flex_shrink * items[item_index].main;
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
        const int64_t denominator = growing ? (int64_t)total_grow : total_shrink_scaled;

        /* First pass: detect and freeze min/max violations without committing the others.
         * The bounds are compared against the EXACT size, scaled by denominator — a truncated size
         * sits inside a max bound that the real one exceeds, which would leave the item
         * flexible and hand it a size past its bound. */
        bool froze_one = false;
        for (int item_index = 0; item_index < item_count; item_index++)
        {
            if (items[item_index].line != line_index || items[item_index].frozen)
                continue;
            const int64_t exact_size =
                (int64_t)items[item_index].main * denominator + flex_share_num(&items[item_index], remaining, growing);
            int64_t bounded = exact_size;
            if (bounded < 0)
                bounded = 0;
            if (items[item_index].main_min != ER_LAYOUT_AUTO
                && bounded < (int64_t)items[item_index].main_min * denominator)
                bounded = (int64_t)items[item_index].main_min * denominator;
            if (items[item_index].main_max != ER_LAYOUT_AUTO
                && bounded > (int64_t)items[item_index].main_max * denominator)
                bounded = (int64_t)items[item_index].main_max * denominator;
            if (bounded < 0)
                bounded = 0;
            if (bounded != exact_size)
            {
                /* A bound is a whole pixel, so the frozen size divides out exactly. */
                items[item_index].main = (int16_t)(bounded / denominator);
                items[item_index].frozen = 1U;
                froze_one = true;
            }
        }
        if (froze_one)
            continue;

        /* No violations: commit the distributed sizes to every remaining flexible item.
         *
         * Each item spans from one rounded edge to the next, so a leftover that does not
         * divide evenly lands on whichever item straddles the half pixel rather than being
         * dropped at the end of the line. `carry`/`edge_remainder` track the running edge exactly; the
         * frozen items in between are whole pixels and never move the fraction. */
        int32_t carry = 0;
        int64_t edge_remainder = 0;
        int32_t prev_edge = 0;
        for (int item_index = 0; item_index < item_count; item_index++)
        {
            if (items[item_index].line != line_index || items[item_index].frozen)
                continue;
            const int64_t share_numerator = flex_share_num(&items[item_index], remaining, growing);
            int64_t share_quotient = share_numerator / denominator;
            int64_t share_remainder =
                share_numerator % denominator; /* Same sign as share_numerator; renormalise to a floor division. */
            if (share_remainder < 0)
            {
                share_quotient--;
                share_remainder += denominator;
            }
            carry += (int32_t)share_quotient;
            edge_remainder += share_remainder;
            if (edge_remainder >= denominator)
            {
                edge_remainder -= denominator;
                carry++;
            }
            const int32_t edge = edge_round(carry, edge_remainder, denominator, reversed_main);
            int32_t flexed_size = items[item_index].main + (edge - prev_edge);
            prev_edge = edge;
            if (flexed_size < 0)
                flexed_size = 0;
            items[item_index].main =
                clamp_size((int16_t)flexed_size, items[item_index].main_min, items[item_index].main_max);
        }
        break;
    }
}

static void measure_content(const uint16_t tag, const int16_t available_width, int16_t* out_width, int16_t* out_height);

/**
 * @brief Measures a child's intrinsic content size at most once, on first use.
 *
 * Most Pass 1 children resolve both axes from explicit size / percentage / flex_basis
 * and never touch the intrinsic size at all — measuring them anyway would recurse their
 * whole subtree for nothing. Callers thread `measured`/`intrinsic_width`/`intrinsic_height` through the main-axis and
 * cross-axis resolution so the (potentially expensive) measure_content() call happens on
 * demand and is shared between the two axes when both need it.
 *
 * @param[in]      tag               Tag of the child to measure.
 * @param[in]      available_width   Width the child may occupy, or ER_LAYOUT_AUTO when unconstrained.
 * @param[in,out]  measured          Set to true once this call has measured; skips re-measuring.
 * @param[out]     intrinsic_width   Receives the intrinsic width (only written on the first call).
 * @param[out]     intrinsic_height  Receives the intrinsic height (only written on the first call).
 */
static void measure_lazy(const uint16_t tag,
                         const int16_t available_width,
                         bool* measured,
                         int16_t* intrinsic_width,
                         int16_t* intrinsic_height)
{
    if (*measured)
        return;
    measure_content(tag, available_width, intrinsic_width, intrinsic_height);
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
 * @param[in] container_spec  The container's layout spec.
 * @param[in] child_spec      The child's layout spec.
 * @param[in] inner_width     The container's content-box width, or ER_LAYOUT_AUTO when unknown.
 *
 * @return The width offered, or ER_LAYOUT_AUTO when the child is measured unconstrained.
 */
static int16_t
offered_width(const ERLayoutSpec* container_spec, const ERLayoutSpec* child_spec, const int16_t inner_width)
{
    if (inner_width == ER_LAYOUT_AUTO)
        return ER_LAYOUT_AUTO;
    if (child_spec->width_pct > 0.0f)
        return round_px((float)inner_width * child_spec->width_pct / PERCENT_SCALE);
    if (child_spec->width != ER_LAYOUT_AUTO
        || (is_row_dir(container_spec->flex_direction) && container_spec->overflow == ER_OVERFLOW_SCROLL))
        return ER_LAYOUT_AUTO;
    const int16_t margin_horizontal = (int16_t)(edge_or(child_spec->margin_left, child_spec->margin)
                                                + edge_or(child_spec->margin_right, child_spec->margin));
    return clamp_size((int16_t)(inner_width > margin_horizontal ? inner_width - margin_horizontal : 0),
                      child_spec->min_width,
                      child_spec->max_width);
}

/**
 * @brief The main size aspect_ratio gives an item from its definite cross size.
 *
 * @param[in] child_spec  The item's layout spec.
 * @param[in] is_row      Whether the main axis is horizontal, so the size returned is a width.
 * @param[in] cross_size  Container content-box extent on the cross axis, or ER_LAYOUT_AUTO when unknown.
 *
 * @return The main size, or ER_LAYOUT_AUTO when the item has no aspect_ratio or no definite cross size.
 */
static int16_t aspect_main_size(const ERLayoutSpec* child_spec, const bool is_row, const int16_t cross_size)
{
    if (child_spec->aspect_ratio <= 0.0f)
        return ER_LAYOUT_AUTO;
    const float cross_percent = is_row ? child_spec->height_pct : child_spec->width_pct;
    int16_t cross = is_row ? child_spec->height : child_spec->width;
    if (cross_percent > 0.0f)
    {
        if (cross_size == ER_LAYOUT_AUTO)
            return ER_LAYOUT_AUTO;
        cross = round_px((float)cross_size * cross_percent / PERCENT_SCALE);
    }
    else if (cross == ER_LAYOUT_AUTO)
        return ER_LAYOUT_AUTO;
    cross = clamp_size(cross,
                       is_row ? child_spec->min_height : child_spec->min_width,
                       is_row ? child_spec->max_height : child_spec->max_width);
    /* aspect_ratio == width / height. */
    return round_px(is_row ? (float)cross * child_spec->aspect_ratio : (float)cross / child_spec->aspect_ratio);
}

/**
 * @brief Whether aspect_ratio gives an item its cross size from its main size.
 *
 * @param[in] child_spec  The item's layout spec.
 * @param[in] is_row      Whether the container's main axis is horizontal.
 *
 * @return true when the item has an aspect_ratio and no explicit or percentage cross size.
 */
static bool cross_from_aspect(const ERLayoutSpec* child_spec, const bool is_row)
{
    return child_spec->aspect_ratio > 0.0f && (is_row ? child_spec->height : child_spec->width) == ER_LAYOUT_AUTO
           && (is_row ? child_spec->height_pct : child_spec->width_pct) <= 0.0f;
}

/**
 * @brief The cross size aspect_ratio gives an item from its main size.
 *
 * @param[in] child_spec  The item's layout spec.
 * @param[in] is_row      Whether the main axis is horizontal, so the size returned is a height.
 * @param[in] main        The item's main size.
 *
 * @return The cross size, held to the item's cross-axis min/max.
 */
static int16_t aspect_cross_size(const ERLayoutSpec* child_spec, const bool is_row, const int16_t main)
{
    const float cross = is_row ? (float)main / child_spec->aspect_ratio : (float)main * child_spec->aspect_ratio;
    return clamp_size(round_px(cross),
                      is_row ? child_spec->min_height : child_spec->min_width,
                      is_row ? child_spec->max_height : child_spec->max_width);
}

/**
 * @brief A flex item's base main size — flex_basis_pct (%) > flex_basis (px) > explicit size > aspect ratio >
 * intrinsic.
 *
 * @param[in]      tag               Tag of the item.
 * @param[in]      child_spec        The item's layout spec.
 * @param[in]      is_row            Whether the container's main axis is horizontal.
 * @param[in]      main_size         Container content-box extent on the main axis; percentages resolve against it.
 * @param[in]      cross_size        Container content-box extent on the cross axis, or ER_LAYOUT_AUTO when unknown.
 * @param[in]      available_width   Width offered to the item's measurement, or ER_LAYOUT_AUTO when unconstrained.
 * @param[in,out]  measured          Threaded through measure_lazy(), so the measurement is shared with the cross axis.
 * @param[out]     intrinsic_width   Receives the intrinsic width if this call measures.
 * @param[out]     intrinsic_height  Receives the intrinsic height if this call measures.
 *
 * @return The base main size, held to the item's main-axis min/max.
 */
static int16_t base_main_size(const uint16_t tag,
                              const ERLayoutSpec* child_spec,
                              const bool is_row,
                              const int16_t main_size,
                              const int16_t cross_size,
                              const int16_t available_width,
                              bool* measured,
                              int16_t* intrinsic_width,
                              int16_t* intrinsic_height)
{
    int16_t hypothetical_main;
    if (child_spec->flex_basis_pct > 0.0f)
    {
        hypothetical_main = round_px((float)main_size * child_spec->flex_basis_pct / PERCENT_SCALE);
    }
    else if (child_spec->flex_basis != ER_LAYOUT_AUTO)
    {
        hypothetical_main = child_spec->flex_basis;
    }
    else
    {
        const int16_t main_explicit = is_row ? child_spec->width : child_spec->height;
        const float main_percent = is_row ? child_spec->width_pct : child_spec->height_pct;
        if (main_percent > 0.0f)
            hypothetical_main = round_px((float)main_size * main_percent / PERCENT_SCALE);
        else if (main_explicit != ER_LAYOUT_AUTO)
            hypothetical_main = main_explicit;
        else if ((hypothetical_main = aspect_main_size(child_spec, is_row, cross_size)) == ER_LAYOUT_AUTO)
        {
            measure_lazy(tag, available_width, measured, intrinsic_width, intrinsic_height);
            hypothetical_main = is_row ? *intrinsic_width : *intrinsic_height;
        }
    }
    return clamp_size(hypothetical_main,
                      is_row ? child_spec->min_width : child_spec->min_height,
                      is_row ? child_spec->max_width : child_spec->max_height);
}

/**
 * @brief Re-measures a row item's content height at the width flexing gave it.
 *
 * A row measures its children before it knows how wide they will be, so an item that flexes to
 * another width may wrap its text onto a different number of lines. An item already at the width
 * it was measured at is left alone: measuring again there would give the same answer.
 *
 * @param[in,out] item  The item; `cross` is updated when its height is its content's.
 */
static void remeasure_cross(FlexChild* item)
{
    if (item->measured_width == ER_LAYOUT_AUTO || item->main == item->measured_width)
        return;
    int16_t width = 0, height = 0;
    measure_content(item->tag, item->main, &width, &height);
    item->cross = height;
    item->measured_width = item->main;
}

/**
 * @brief Looks a measurement up in the per-pass cache.
 *
 * @param[in]  tag              Tag of the node.
 * @param[in]  available_width  Width the node is offered, or ER_LAYOUT_AUTO.
 * @param[out] out_width        Receives the cached width on a hit.
 * @param[out] out_height       Receives the cached height on a hit.
 *
 * @return true on a hit.
 */
static bool cache_get(const uint16_t tag, const int16_t available_width, int16_t* out_width, int16_t* out_height)
{
    if (tag >= (uint16_t)ERUI_MAX_NODES)
        return false;
    for (int way = 0; way < MEASURE_CACHE_WAYS; way++)
    {
        const MeasureCacheEntry* entry = &s_measure_cache[tag][way];
        if (entry->generation == s_measure_generation && entry->available_width == available_width)
        {
            *out_width = entry->width;
            *out_height = entry->height;
            return true;
        }
    }
    return false;
}

/**
 * @brief Stores a measurement in the per-pass cache, evicting the node's oldest one.
 *
 * @param[in] tag              Tag of the node.
 * @param[in] available_width  Width the node was offered, or ER_LAYOUT_AUTO.
 * @param[in] width            Measured width.
 * @param[in] height           Measured height.
 */
static void cache_put(const uint16_t tag, const int16_t available_width, const int16_t width, const int16_t height)
{
    if (tag >= (uint16_t)ERUI_MAX_NODES)
        return;
    MeasureCacheEntry* ways = s_measure_cache[tag];
    memmove(&ways[1], &ways[0], sizeof(ways[0]) * (MEASURE_CACHE_WAYS - 1));
    ways[0] = (MeasureCacheEntry){width, height, available_width, s_measure_generation};
}

/**
 * @brief Returns true when a Text node's content holds an explicit line break.
 *
 * @param[in] text_props  The node's text props.
 *
 * @return true when the text, or any of its spans, contains '\n'.
 */
static bool text_has_newline(const ERTextProps* text_props)
{
    if (text_props->span_count == 0U)
        return strchr(text_props->text, '\n') != NULL;
    for (uint8_t span_index = 0; span_index < text_props->span_count; span_index++)
        if (strchr(text_props->spans[span_index].text, '\n') != NULL)
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
 * @param[in]  text_props   The node's text props.
 * @param[in]  break_width  Width the glyphs are drawn in, or 0 when nothing bounds it.
 * @param[out] out_width    Receives the content width in pixels.
 * @param[out] out_height   Receives the content height in pixels.
 */
static void measure_text(const ERTextProps* text_props, const int break_width, int32_t* out_width, int32_t* out_height)
{
    int measured_width = 0, measured_height = 0;
    if (text_props->span_count > 0U)
    {
        /* Styled spans render with per-run weight/spacing — measure them the same way so a
           bold/styled run does not overflow an auto-sized node and clip its trailing glyphs. */
        er_text_measure_spans(text_props->spans,
                              text_props->span_count,
                              text_props->font_size,
                              text_props->font_family,
                              text_props->letter_spacing,
                              text_props->font_weight,
                              &measured_width,
                              &measured_height);
    }
    else
    {
        er_text_measure(text_props->text,
                        text_props->font_size,
                        text_props->font_family,
                        text_props->letter_spacing,
                        text_props->font_weight,
                        &measured_width,
                        &measured_height);
    }
    const int32_t line_height = (text_props->line_height > 0) ? text_props->line_height : measured_height;

    int lines = 1;
    if ((break_width > 0 && measured_width > break_width) || text_has_newline(text_props))
    {
        ERTextRenderParams render_params;
        memset(&render_params, 0, sizeof(render_params));
        render_params.text = text_props->text;
        render_params.font_size = text_props->font_size;
        render_params.font_family = text_props->font_family;
        render_params.number_of_lines = text_props->number_of_lines;
        render_params.font_weight = text_props->font_weight;
        render_params.letter_spacing = text_props->letter_spacing;
        render_params.span_count = text_props->span_count;
        render_params.spans = (text_props->span_count > 0U) ? text_props->spans : NULL;
        int broken = 0;
        er_text_measure_lines(&render_params, break_width, &measured_width, &broken);
        if (broken > 1)
            lines = broken;
    }
    *out_width = measured_width;
    *out_height = line_height * lines;
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
 * @param[in]  node          The row.
 * @param[in]  inner_width   The row's content-box width.
 * @param[in]  inner_height  The row's content-box height, or ER_LAYOUT_AUTO when unknown.
 * @param[in]  main_gap      Gap between adjacent children.
 * @param[out] out_width     Receives the content width.
 * @param[out] out_height    Receives the content height.
 *
 * @return false when the row has no in-flow children or s_scratch has no room; nothing is written then.
 */
static bool measure_flexed_row(const ERNode* node,
                               const int16_t inner_width,
                               const int16_t inner_height,
                               const int16_t main_gap,
                               int16_t* out_width,
                               int16_t* out_height)
{
    int count = 0;
    for (uint16_t child_tag = node->first_child_tag; child_tag != ER_INVALID_TAG;)
    {
        const ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        if (child->layout.position != ER_POS_ABSOLUTE && child->layout.display != ER_DISPLAY_NONE)
            count++;
        child_tag = child->next_sibling_tag;
    }
    if (count == 0 || count > s_measure_top)
        return false;

    const int top = s_measure_top;
    s_measure_top -= count;
    FlexChild* items = &s_scratch[s_measure_top];

    int filled = 0;
    for (uint16_t child_tag = node->first_child_tag; child_tag != ER_INVALID_TAG && filled < count;)
    {
        const ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        if (child->layout.position != ER_POS_ABSOLUTE && child->layout.display != ER_DISPLAY_NONE)
        {
            const ERLayoutSpec* child_spec = &child->layout;
            const int16_t available_width = offered_width(&node->layout, child_spec, inner_width);
            bool measured = false;
            int16_t intrinsic_width = 0, intrinsic_height = 0;

            FlexChild* item = &items[filled++];
            memset(item, 0, sizeof(*item));
            item->tag = child_tag;
            item->main = base_main_size(child_tag,
                                        child_spec,
                                        true,
                                        inner_width,
                                        inner_height,
                                        available_width,
                                        &measured,
                                        &intrinsic_width,
                                        &intrinsic_height);
            measure_lazy(child_tag, available_width, &measured, &intrinsic_width, &intrinsic_height);
            item->cross = intrinsic_height;
            item->measured_width = (child_spec->height == ER_LAYOUT_AUTO) ? intrinsic_width : ER_LAYOUT_AUTO;
            item->margin_main_start = edge_or(child_spec->margin_left, child_spec->margin);
            item->margin_main_end = edge_or(child_spec->margin_right, child_spec->margin);
            item->margin_cross_start = edge_or(child_spec->margin_top, child_spec->margin);
            item->margin_cross_end = edge_or(child_spec->margin_bottom, child_spec->margin);
            item->flex_grow = child_spec->flex_grow;
            item->flex_shrink = child_spec->flex_shrink;
            item->main_min = child_spec->min_width;
            item->main_max = child_spec->max_width;
        }
        child_tag = child->next_sibling_tag;
    }

    /* A scroll container's children keep their own sizes, as in Pass 3. */
    if (node->layout.overflow != ER_OVERFLOW_SCROLL)
        resolve_flex_line(items, filled, 0, inner_width, main_gap, is_reverse_dir(node->layout.flex_direction));

    int32_t main_sum = (filled > 1) ? (int32_t)main_gap * (filled - 1) : 0;
    int16_t cross_max = 0;
    for (int item_index = 0; item_index < filled; item_index++)
    {
        const ERNode* child = er_get_node(items[item_index].tag);
        if (child && cross_from_aspect(&child->layout, true))
            items[item_index].cross = aspect_cross_size(&child->layout, true, items[item_index].main);
        else
            remeasure_cross(&items[item_index]);
        main_sum +=
            (int32_t)items[item_index].main + items[item_index].margin_main_start + items[item_index].margin_main_end;
        const int32_t outer_height = (int32_t)items[item_index].cross + items[item_index].margin_cross_start
                                     + items[item_index].margin_cross_end;
        if (outer_height > cross_max)
            cross_max = (int16_t)(outer_height > INT16_MAX ? INT16_MAX : outer_height);
    }

    s_measure_top = top;
    *out_width = (int16_t)(main_sum > INT16_MAX ? INT16_MAX : (main_sum < 0 ? 0 : main_sum));
    *out_height = cross_max;
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
 * When the width is known — an explicit width, or `available_width` from the parent — a Text breaks into
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
 * @param[in]  tag              Tag of the node to measure.
 * @param[in]  available_width  Width the node may occupy, or ER_LAYOUT_AUTO when unconstrained.
 * @param[out] out_width        Receives the intrinsic width in pixels.
 * @param[out] out_height       Receives the intrinsic height in pixels.
 */
static void measure_content(const uint16_t tag, const int16_t available_width, int16_t* out_width, int16_t* out_height)
{
    /* Per-pass memoisation: measure_content(tag, available_width) is a pure function of that node's own
     * subtree for the duration of one er_layout_compute() call, so a cache hit here is
     * exactly what turns the O(depth) remeasurement into O(1) per node per pass. */
    if (cache_get(tag, available_width, out_width, out_height))
        return;

    ERNode* node = er_get_node(tag);
    if (!node)
    {
        *out_width = 0;
        *out_height = 0;
        return;
    }

    const ERLayoutSpec* spec = &node->layout;
    int16_t explicit_width = spec->width;
    int16_t explicit_height = spec->height;
    /* aspect_ratio turns a definite size on one axis into one on the other. A percentage width
     * arrives already resolved as available_width. */
    if (spec->aspect_ratio > 0.0f)
    {
        if (explicit_width == ER_LAYOUT_AUTO && spec->width_pct > 0.0f)
            explicit_width = available_width;
        if (explicit_width == ER_LAYOUT_AUTO)
            explicit_width = aspect_main_size(spec, true, ER_LAYOUT_AUTO);
        else if (explicit_height == ER_LAYOUT_AUTO)
            explicit_height =
                aspect_cross_size(spec, true, clamp_size(explicit_width, spec->min_width, spec->max_width));
    }

    /* Text leaf: measure the glyph run, then add the node's own padding — the glyphs are this node's
     * content, so an auto axis has to grow around them exactly as a container grows around a child
     * (the compositor insets the render clip by the same padding, so the two agree). An EXPLICIT
     * width/height already includes its padding, as everywhere else here. Both axes explicit means
     * the measurement result is never consulted below, so skip it. */
    if (node->type == ER_NODE_TEXT)
    {
        int16_t node_width, node_height;
        if (explicit_width != ER_LAYOUT_AUTO && explicit_height != ER_LAYOUT_AUTO)
        {
            node_width = explicit_width;
            node_height = explicit_height;
        }
        else
        {
            const ERPadding text_padding = er_layout_padding(spec);
            /* The glyphs are drawn in the node's content box: its own width, else the width it is
             * offered, held to its min/max. Nothing is drawn in a box with no width, so a box no
             * wider than its padding is treated as unbounded rather than as one glyph per line. */
            int16_t box_width = explicit_width;
            if (box_width == ER_LAYOUT_AUTO)
                box_width = (available_width != ER_LAYOUT_AUTO)
                                ? clamp_size(available_width, spec->min_width, spec->max_width)
                                : spec->max_width;
            const int break_width = (box_width != ER_LAYOUT_AUTO && box_width > text_padding.left + text_padding.right)
                                        ? box_width - text_padding.left - text_padding.right
                                        : 0;

            int32_t text_width = 0, text_height = 0;
            measure_text(&node->props.text, break_width, &text_width, &text_height);
            text_width += text_padding.left + text_padding.right;
            text_height += text_padding.top + text_padding.bottom;
            node_width = (explicit_width != ER_LAYOUT_AUTO)
                             ? explicit_width
                             : (int16_t)(text_width > INT16_MAX ? INT16_MAX : text_width);
            node_height = (explicit_height != ER_LAYOUT_AUTO)
                              ? explicit_height
                              : (int16_t)(text_height > INT16_MAX ? INT16_MAX : text_height);
        }
        *out_width = clamp_size(node_width, spec->min_width, spec->max_width);
        *out_height = clamp_size(node_height, spec->min_height, spec->max_height);
        cache_put(tag, available_width, *out_width, *out_height);
        return;
    }

    /* Container / leaf View: derive content size from in-flow children (only when an axis is auto). */
    const ERPadding padding = er_layout_padding(spec);
    int16_t content_width = 0, content_height = 0;
    if (node->first_child_tag != ER_INVALID_TAG
        && (explicit_width == ER_LAYOUT_AUTO || explicit_height == ER_LAYOUT_AUTO))
    {
        const bool is_row = is_row_dir(spec->flex_direction);
        const int16_t main_gap = is_row ? edge_or(spec->column_gap, spec->gap) : edge_or(spec->row_gap, spec->gap);
        const int16_t cross_gap = is_row ? edge_or(spec->row_gap, spec->gap) : edge_or(spec->column_gap, spec->gap);
        /* A node's own min/max width bounds what it is offered, so lines break where layout breaks them. */
        const int16_t offered_limit = (available_width == ER_LAYOUT_AUTO)
                                          ? ER_LAYOUT_AUTO
                                          : clamp_size(available_width, spec->min_width, spec->max_width);
        const int16_t box_width = (explicit_width != ER_LAYOUT_AUTO) ? explicit_width : offered_limit;
        const int16_t inner_width =
            (box_width == ER_LAYOUT_AUTO)
                ? ER_LAYOUT_AUTO
                : (int16_t)(box_width > padding.left + padding.right ? box_width - padding.left - padding.right : 0);
        /* A wrapping row with a known width breaks into lines; the main axis is then the widest
         * line and the cross axis the summed line heights plus the gaps between them. */
        const bool wraps = is_row && spec->flex_wrap != ER_WRAP_NOWRAP && inner_width != ER_LAYOUT_AUTO;
        const int16_t inner_height = (explicit_height == ER_LAYOUT_AUTO)
                                         ? ER_LAYOUT_AUTO
                                         : (int16_t)(explicit_height > padding.top + padding.bottom
                                                         ? explicit_height - padding.top - padding.bottom
                                                         : 0);
        /* A row that knows its width flexes its children first: their heights depend on the widths they get. */
        const bool flexed =
            is_row && !wraps && inner_width != ER_LAYOUT_AUTO
            && measure_flexed_row(node, inner_width, inner_height, main_gap, &content_width, &content_height);
        if (!flexed)
        {
            int32_t main_sum = 0;
            int16_t cross_max = 0;
            int32_t wrapped_cross = 0;
            int32_t widest_line = 0;
            int count = 0;
            int line_count = 0;
            for (uint16_t child_tag = node->first_child_tag; child_tag != ER_INVALID_TAG;)
            {
                ERNode* child = er_get_node(child_tag);
                if (!child)
                    break;
                if (child->layout.position != ER_POS_ABSOLUTE && child->layout.display != ER_DISPLAY_NONE)
                {
                    int16_t child_width = 0, child_height = 0;
                    const ERLayoutSpec* child_spec = &child->layout;
                    const int16_t margin_horizontal =
                        (int16_t)(edge_or(child_spec->margin_left, child_spec->margin)
                                  + edge_or(child_spec->margin_right, child_spec->margin));
                    measure_content(
                        child_tag, offered_width(spec, child_spec, inner_width), &child_width, &child_height);
                    const int16_t outer_width = (int16_t)(child_width + margin_horizontal);
                    const int16_t outer_height =
                        (int16_t)(child_height + edge_or(child_spec->margin_top, child_spec->margin)
                                  + edge_or(child_spec->margin_bottom, child_spec->margin));
                    if (wraps)
                    {
                        if (line_count > 0 && main_sum + main_gap + outer_width > inner_width)
                        {
                            wrapped_cross += cross_max + cross_gap;
                            if (main_sum > widest_line)
                                widest_line = main_sum;
                            main_sum = 0;
                            cross_max = 0;
                            line_count = 0;
                        }
                        main_sum += (line_count > 0 ? main_gap : 0) + outer_width;
                        if (outer_height > cross_max)
                            cross_max = outer_height;
                        line_count++;
                    }
                    else if (is_row)
                    {
                        main_sum += outer_width;
                        if (outer_height > cross_max)
                            cross_max = outer_height;
                    }
                    else
                    {
                        main_sum += outer_height;
                        if (outer_width > cross_max)
                            cross_max = outer_width;
                    }
                    count++;
                }
                child_tag = child->next_sibling_tag;
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
                content_width = (int16_t)main_sum;
                content_height = cross_max;
            }
            else
            {
                content_height = (int16_t)main_sum;
                content_width = cross_max;
            }
        }
    }

    const int16_t intrinsic_width =
        (explicit_width != ER_LAYOUT_AUTO) ? explicit_width : (int16_t)(content_width + padding.left + padding.right);
    const int16_t intrinsic_height = (explicit_height != ER_LAYOUT_AUTO)
                                         ? explicit_height
                                         : (int16_t)(content_height + padding.top + padding.bottom);
    *out_width = clamp_size(intrinsic_width, spec->min_width, spec->max_width);
    *out_height = clamp_size(intrinsic_height, spec->min_height, spec->max_height);
    cache_put(tag, available_width, *out_width, *out_height);
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
 * @param[in] tag     Tag of the node to lay out.
 * @param[in] width   Width allocated to this node in pixels.
 * @param[in] height  Height allocated to this node in pixels.
 * @param[in] x       Screen X origin of this node.
 * @param[in] y       Screen Y origin of this node.
 */
static void
compute_layout(const uint16_t tag, const int16_t width, const int16_t height, const int16_t x, const int16_t y)
{
    ERNode* node = er_get_node(tag);
    if (!node)
        return;

    if (node->layout.display == ER_DISPLAY_NONE)
    {
        node->computed.x = 0;
        node->computed.y = 0;
        node->computed.w = 0;
        node->computed.h = 0;
        return;
    }

    node->computed.x = x;
    node->computed.y = y;
    node->computed.w = width;
    node->computed.h = height;

    if (node->first_child_tag == ER_INVALID_TAG)
        return;

    const ERLayoutSpec* spec = &node->layout;

    /* Padding (per-edge wins over shorthand). */
    const ERPadding padding = er_layout_padding(spec);
    const int16_t padding_left = padding.left;
    const int16_t padding_right = padding.right;
    const int16_t padding_top = padding.top;
    const int16_t padding_bottom = padding.bottom;

    int16_t content_width = (int16_t)(width - padding_left - padding_right);
    int16_t content_height = (int16_t)(height - padding_top - padding_bottom);
    if (content_width < 0)
        content_width = 0;
    if (content_height < 0)
        content_height = 0;
    const int16_t content_x = (int16_t)(x + padding_left);
    const int16_t content_y = (int16_t)(y + padding_top);

    const bool is_row = is_row_dir(spec->flex_direction);
    const bool reversed_main = is_reverse_dir(spec->flex_direction);
    const int16_t main_size = is_row ? content_width : content_height;
    const int16_t cross_avail = is_row ? content_height : content_width;
    const int16_t main_gap = is_row ? edge_or(spec->column_gap, spec->gap) : edge_or(spec->row_gap, spec->gap);
    const int16_t cross_gap = is_row ? edge_or(spec->row_gap, spec->gap) : edge_or(spec->column_gap, spec->gap);

    /*--------------------------------------------------------------------------
     * Pass 1 — collect in-flow children; assign hypothetical sizes.
     *------------------------------------------------------------------------*/
    int inflow_count = 0;
    for (uint16_t child_tag = node->first_child_tag; child_tag != ER_INVALID_TAG;)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        if (child->layout.position != ER_POS_ABSOLUTE && child->layout.display != ER_DISPLAY_NONE)
        {
            const ERLayoutSpec* child_spec = &child->layout;

            /* Intrinsic content size — Text measures its glyph run; containers measure their
             * children (so an auto-sized container fits its content instead of collapsing to 0).
             * Measured lazily below: most children resolve both axes from an explicit size,
             * percentage, or flex_basis and never need it, and measuring recurses the child's
             * whole subtree, so paying for it unconditionally here is exactly the O(depth)
             * remeasurement this cache/laziness combination avoids. */
            bool measured = false;
            int16_t intrinsic_width = 0, intrinsic_height = 0;

            /* Width the child may occupy, offered to its measurement so text and wrapping rows
             * inside it break into lines at the content box's edge. */
            const int16_t available_width = offered_width(spec, child_spec, content_width);

            int16_t hypothetical_main = base_main_size(child_tag,
                                                       child_spec,
                                                       is_row,
                                                       main_size,
                                                       cross_avail,
                                                       available_width,
                                                       &measured,
                                                       &intrinsic_width,
                                                       &intrinsic_height);
            const int16_t main_min = is_row ? child_spec->min_width : child_spec->min_height;
            const int16_t main_max = is_row ? child_spec->max_width : child_spec->max_height;

            /* Base cross size (stretch may override later if still auto). When aspect_ratio
             * will drive the cross size after flexing, skip measuring. */
            const int16_t cross_explicit = is_row ? child_spec->height : child_spec->width;
            const float cross_percent = is_row ? child_spec->height_pct : child_spec->width_pct;
            const int16_t cross_min = is_row ? child_spec->min_height : child_spec->min_width;
            const int16_t cross_max = is_row ? child_spec->max_height : child_spec->max_width;
            const bool cross_via_aspect = cross_from_aspect(child_spec, is_row);
            int16_t hypothetical_cross;
            int16_t measured_width = ER_LAYOUT_AUTO;
            if (cross_percent > 0.0f)
                hypothetical_cross = round_px((float)cross_avail * cross_percent / PERCENT_SCALE);
            else if (cross_explicit != ER_LAYOUT_AUTO)
                hypothetical_cross = cross_explicit;
            else if (cross_via_aspect)
                hypothetical_cross = 0; /* placeholder — set from the flexed main size after Pass 3 */
            else
            {
                measure_lazy(child_tag, available_width, &measured, &intrinsic_width, &intrinsic_height);
                hypothetical_cross = is_row ? intrinsic_height : intrinsic_width;
                if (is_row)
                    measured_width = intrinsic_width;
            }
            hypothetical_cross = clamp_size(hypothetical_cross, cross_min, cross_max);

            /* Per-edge margins. */
            const int16_t margin_left = edge_or(child_spec->margin_left, child_spec->margin);
            const int16_t margin_right = edge_or(child_spec->margin_right, child_spec->margin);
            const int16_t margin_top = edge_or(child_spec->margin_top, child_spec->margin);
            const int16_t margin_bottom = edge_or(child_spec->margin_bottom, child_spec->margin);

            FlexChild* item = &s_scratch[inflow_count++];
            item->tag = child_tag;
            item->main = hypothetical_main;
            item->cross = hypothetical_cross;
            item->margin_main_start = is_row ? margin_left : margin_top;
            item->margin_main_end = is_row ? margin_right : margin_bottom;
            item->margin_cross_start = is_row ? margin_top : margin_left;
            item->margin_cross_end = is_row ? margin_bottom : margin_right;
            item->flex_grow = child_spec->flex_grow;
            item->flex_shrink = child_spec->flex_shrink;
            item->main_min = main_min;
            item->main_max = main_max;
            item->measured_width = measured_width;
            item->frozen = 0U;
            item->line = 0;
            item->align = (child_spec->align_self != ER_ALIGN_AUTO) ? child_spec->align_self : spec->align_items;
            if (item->align == ER_ALIGN_AUTO)
                item->align = ER_ALIGN_STRETCH;
        }
        child_tag = child->next_sibling_tag;
    }

    /*--------------------------------------------------------------------------
     * Pass 2 — wrap children into lines.
     *------------------------------------------------------------------------*/
    int line_count = (inflow_count > 0) ? 1 : 0;
    if (spec->flex_wrap != ER_WRAP_NOWRAP && inflow_count > 0)
    {
        int line = 0;
        int16_t line_used = 0;
        bool first_on_line = true;
        for (int item_index = 0; item_index < inflow_count; item_index++)
        {
            const int16_t outer = (int16_t)(s_scratch[item_index].main + s_scratch[item_index].margin_main_start
                                            + s_scratch[item_index].margin_main_end);
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
            s_scratch[item_index].line = (uint8_t)line;
        }
        line_count = line + 1;
    }

    /*--------------------------------------------------------------------------
     * Pass 3 — per line: resolve flex_grow / flex_shrink against free space.
     *
     * For overflow:scroll containers the main axis is unbounded: children keep
     * their natural sizes instead of growing or shrinking to fit the viewport.
     * This matches React Native's ScrollView behaviour where content overflows
     * the viewport to produce a scrollable virtual content size.
     *------------------------------------------------------------------------*/
    const bool is_scroll = (spec->overflow == ER_OVERFLOW_SCROLL);
    if (!is_scroll)
    {
        for (int line_index = 0; line_index < line_count; line_index++)
            resolve_flex_line(s_scratch, inflow_count, line_index, main_size, main_gap, reversed_main);
    }

    /* aspect_ratio gives the cross size from the main size each item flexed to. */
    for (int item_index = 0; item_index < inflow_count; item_index++)
    {
        const ERNode* child = er_get_node(s_scratch[item_index].tag);
        if (child && cross_from_aspect(&child->layout, is_row))
            s_scratch[item_index].cross = aspect_cross_size(&child->layout, is_row, s_scratch[item_index].main);
    }

    /*--------------------------------------------------------------------------
     * Pass 3b — in a row, re-measure each child whose height is its content's at the width it
     * flexed to: a Text given less room than its single line wraps onto more lines, and a line is as
     * tall as its tallest child. A stretched child of a single-line row takes the row's height
     * whatever it measures, so it is skipped.
     *------------------------------------------------------------------------*/
    if (is_row)
    {
        for (int item_index = 0; item_index < inflow_count; item_index++)
        {
            if (spec->flex_wrap == ER_WRAP_NOWRAP && s_scratch[item_index].align == ER_ALIGN_STRETCH)
                continue;
            remeasure_cross(&s_scratch[item_index]);
        }
    }

    /*--------------------------------------------------------------------------
     * Pass 4 — per line: cross-axis size = max child outer cross-size.
     *------------------------------------------------------------------------*/
    for (int line_index = 0; line_index < line_count; line_index++)
    {
        int16_t line_cross = 0;
        for (int item_index = 0; item_index < inflow_count; item_index++)
        {
            if (s_scratch[item_index].line != line_index)
                continue;
            const int16_t outer = (int16_t)(s_scratch[item_index].cross + s_scratch[item_index].margin_cross_start
                                            + s_scratch[item_index].margin_cross_end);
            if (outer > line_cross)
                line_cross = outer;
        }
        s_line_cross[line_index] = line_cross;
    }
    if (spec->flex_wrap == ER_WRAP_NOWRAP && line_count == 1)
        s_line_cross[0] = cross_avail;

    int16_t total_cross = 0;
    for (int line_index = 0; line_index < line_count; line_index++)
    {
        if (line_index > 0)
            total_cross = (int16_t)(total_cross + cross_gap);
        total_cross = (int16_t)(total_cross + s_line_cross[line_index]);
    }

    /*--------------------------------------------------------------------------
     * alignContent — distribute leftover cross space among wrap lines.
     *
     * Only meaningful for a multi-line (wrap) container whose lines do not already fill the cross
     * axis. Each mode is one exact fraction: line `line_index` starts (line_offset_start + line_index *
     * line_offset_step) / line_offset_denominator past where flex-start would put it, and Pass 5 rounds that once.
     *
     * STRETCH is the same shape — growing every line also pushes each later line's start — so it is
     * just line_offset_step. The growth is kept separately too, since a line's extent also sizes a stretched
     * child and positions a centred one.
     *------------------------------------------------------------------------*/
    int32_t line_offset_start = 0;       /**< Numerator of line 0's cross offset. */
    int32_t line_offset_step = 0;        /**< Numerator added per line. */
    int32_t line_offset_denominator = 1; /**< Shared denominator; always > 0. */
    int32_t line_stretch = 0;            /**< STRETCH: cross space added to EVERY line, over line_offset_denominator. */
    if (spec->flex_wrap != ER_WRAP_NOWRAP && line_count > 1)
    {
        int16_t free_cross = (int16_t)(cross_avail - total_cross);
        if (free_cross < 0)
            free_cross = 0;

        if (free_cross > 0)
        {
            switch (spec->align_content)
            {
                case ER_ALIGN_CONTENT_FLEX_END:
                    line_offset_start = free_cross;
                    break;
                case ER_ALIGN_CONTENT_CENTER:
                    line_offset_start = free_cross;
                    line_offset_denominator = 2;
                    break;
                case ER_ALIGN_CONTENT_SPACE_BETWEEN:
                    line_offset_step = free_cross;
                    line_offset_denominator = line_count - 1;
                    break;
                case ER_ALIGN_CONTENT_SPACE_AROUND:
                    line_offset_start = free_cross;
                    line_offset_step = 2 * (int32_t)free_cross;
                    line_offset_denominator = 2 * line_count;
                    break;
                case ER_ALIGN_CONTENT_STRETCH:
                    line_offset_step = free_cross;
                    line_offset_denominator = line_count;
                    line_stretch = free_cross;
                    break;
                default: /* ER_ALIGN_CONTENT_FLEX_START — lines packed at the cross-start. */
                    break;
            }
        }
    }

    /*--------------------------------------------------------------------------
     * Pass 5 — main-axis positions (justifyContent) + cross-axis (alignSelf).
     *------------------------------------------------------------------------*/
    const bool reversed_cross = (spec->flex_wrap == ER_WRAP_WRAP_REVERSE);
    const int32_t cross_denominator = 2 * line_offset_denominator; /* The 2 is alignSelf: center halves the leftover. */
    int32_t line_cross_start = 0; /* Whole-pixel cross start of line line_index (unstretched). */
    for (int line_index = 0; line_index < line_count; line_index++)
    {
        int32_t line_used = 0;
        int count = 0;
        for (int item_index = 0; item_index < inflow_count; item_index++)
        {
            if (s_scratch[item_index].line != line_index)
                continue;
            line_used += s_scratch[item_index].main + s_scratch[item_index].margin_main_start
                         + s_scratch[item_index].margin_main_end;
            count++;
        }
        if (count > 1)
            line_used += (count - 1) * main_gap;

        const int32_t remaining = (int32_t)main_size - line_used;
        /* An overflowing line keeps the sign for centre and flex-end, so a child bigger than the box
         * overhangs the way it does when it is placed as an absolute. The distribution modes have
         * nothing to spread once the line is full, and fall back to flex-start. */
        const int32_t spread = remaining > 0 ? remaining : 0;

        /* justifyContent as one exact fraction per item: item placed_index sits (justify_start + placed_index *
         * justify_step) / justify_denominator past where flex-start would have put it. The leading offset and the
         * between-item step share one fraction on purpose — rounding a step and then accumulating it drifts. */
        int32_t justify_start = 0;
        int32_t justify_step = 0;
        int32_t justify_denominator = 1;
        switch (spec->justify_content)
        {
            case ER_JUSTIFY_CENTER:
                justify_start = remaining;
                justify_denominator = 2;
                break;
            case ER_JUSTIFY_FLEX_END:
                justify_start = remaining;
                break;
            case ER_JUSTIFY_SPACE_BETWEEN:
                if (count > 1)
                {
                    justify_step = spread;
                    justify_denominator = count - 1;
                }
                break;
            case ER_JUSTIFY_SPACE_AROUND:
                if (count > 0)
                {
                    /* Half a unit before the first item, a whole unit between each pair. */
                    justify_start = spread;
                    justify_step = 2 * spread;
                    justify_denominator = 2 * count;
                }
                break;
            case ER_JUSTIFY_SPACE_EVENLY:
                if (count > 0)
                {
                    justify_start = spread;
                    justify_step = spread;
                    justify_denominator = count + 1;
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
        int32_t placed_index = 0;
        for (int item_index = 0; item_index < inflow_count; item_index++)
        {
            if (s_scratch[item_index].line != line_index)
                continue;

            int32_t position = cursor + s_scratch[item_index].margin_main_start;
            int32_t numerator = justify_start + placed_index * justify_step;
            if (reversed_main)
            {
                /* Mirror the item's OUTER box and re-seat the item inside it, so the margin that
                 * leads is the one on the edge the axis starts from. */
                position = main_size - cursor - s_scratch[item_index].margin_main_end - s_scratch[item_index].main;
                numerator = -numerator;
            }
            s_scratch[item_index].main_pos = (int16_t)(position + div_round(numerator, justify_denominator));

            cursor += (int32_t)s_scratch[item_index].margin_main_start + s_scratch[item_index].main
                      + s_scratch[item_index].margin_main_end + main_gap;
            placed_index++;
        }

        /* Cross-axis positions via alignSelf. Three fractions share cross_denominator — the line's
         * alignContent offset, the remainder of a stretched line's growth, and the half that centring
         * takes — and are rounded together at the end of the loop body. */
        /* A stretched line's SIZE comes off the pixel grid the way a flex item's does — the
         * difference of its two rounded cross edges — so the leftover lands on the line that
         * straddles the half pixel instead of being dropped at the far edge. What that rounding left
         * over rides along in the numerators below, so what sits INSIDE the line is still placed
         * exactly. wrap-reverse measures the edges from the far side, so it mirrors them first. */
        const int32_t grow_near =
            reversed_cross ? -(int32_t)(line_index + 1) * line_stretch : (int32_t)line_index * line_stretch;
        const int32_t grow_far =
            reversed_cross ? -(int32_t)line_index * line_stretch : (int32_t)(line_index + 1) * line_stretch;
        const int16_t grow_pixels =
            (int16_t)(div_round(grow_far, line_offset_denominator) - div_round(grow_near, line_offset_denominator));
        const int32_t grow_remainder = line_stretch - (int32_t)grow_pixels * line_offset_denominator;

        const int16_t this_cross = (int16_t)(s_line_cross[line_index] + grow_pixels);
        const int32_t line_numerator = 2 * (line_offset_start + (int32_t)line_index * line_offset_step);
        for (int item_index = 0; item_index < inflow_count; item_index++)
        {
            if (s_scratch[item_index].line != line_index)
                continue;

            int16_t inner = (int16_t)(this_cross - s_scratch[item_index].margin_cross_start
                                      - s_scratch[item_index].margin_cross_end);
            if (inner < 0)
                inner = 0;

            int32_t position = s_scratch[item_index].margin_cross_start; /* Whole pixels, from the line's start. */
            int32_t numerator = 0; /* Sub-pixel remainder, over cross_denominator. */
            switch (s_scratch[item_index].align)
            {
                case ER_ALIGN_STRETCH:
                {
                    /* Stretch fills cross-axis only when size was auto (no explicit px or %, and no
                     * aspect_ratio giving it, as in Yoga). */
                    const ERNode* child = er_get_node(s_scratch[item_index].tag);
                    if (child)
                    {
                        const int16_t child_cross_size = is_row ? child->layout.height : child->layout.width;
                        const float child_cross_percent = is_row ? child->layout.height_pct : child->layout.width_pct;
                        if (child_cross_size == ER_LAYOUT_AUTO && child_cross_percent <= 0.0f
                            && child->layout.aspect_ratio <= 0.0f)
                        {
                            const int16_t child_min = is_row ? child->layout.min_height : child->layout.min_width;
                            const int16_t child_max = is_row ? child->layout.max_height : child->layout.max_width;
                            s_scratch[item_index].cross = clamp_size(inner, child_min, child_max);
                        }
                    }
                    break;
                }
                case ER_ALIGN_FLEX_START:
                    break;
                case ER_ALIGN_CENTER:
                    numerator =
                        (int32_t)(inner - s_scratch[item_index].cross) * line_offset_denominator + grow_remainder;
                    break;
                case ER_ALIGN_FLEX_END:
                    position = this_cross - s_scratch[item_index].margin_cross_end - s_scratch[item_index].cross;
                    numerator = 2 * grow_remainder;
                    break;
                default:
                    break;
            }

            position += line_cross_start;
            numerator += line_numerator;

            /* wrap-reverse mirrors about the CONTENT box, not about the lines' own extent, which
             * would reverse their order without moving the block off the cross-start. alignContent
             * has already placed the block, so the mirror just carries it along. Lines that overflow
             * mirror to negative positions rather than clamping. Mirror before rounding, as above. */
            if (reversed_cross)
            {
                position = cross_avail - position - s_scratch[item_index].cross;
                numerator = -numerator;
            }
            s_scratch[item_index].cross_pos = (int16_t)(position + div_round(numerator, cross_denominator));
        }

        /* line_offset_step carries the stretch growth, so this advances by the line's unstretched extent. */
        line_cross_start += (int32_t)s_line_cross[line_index] + cross_gap;
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
    for (int item_index = 0; item_index < inflow_count; item_index++)
    {
        ERNode* child = er_get_node(s_scratch[item_index].tag);
        if (!child)
            continue;

        int16_t child_x, child_y, child_width, child_height;
        if (is_row)
        {
            child_x = (int16_t)(content_x + s_scratch[item_index].main_pos);
            child_y = (int16_t)(content_y + s_scratch[item_index].cross_pos);
            child_width = s_scratch[item_index].main;
            child_height = s_scratch[item_index].cross;
        }
        else
        {
            child_x = (int16_t)(content_x + s_scratch[item_index].cross_pos);
            child_y = (int16_t)(content_y + s_scratch[item_index].main_pos);
            child_width = s_scratch[item_index].cross;
            child_height = s_scratch[item_index].main;
        }

        /* Relative-position offsets (left/right/top/bottom shift in flow). A percentage measures against
         * this parent's CONTENT box, not the padding box an absolute's insets resolve against. */
        const ERLayoutSpec* child_spec = &child->layout;
        float inset = 0.0f;
        if (inset_px(child_spec->left, child_spec->left_pct, content_width, &inset))
            child_x = round_px((float)child_x + inset);
        else if (inset_px(child_spec->right, child_spec->right_pct, content_width, &inset))
            child_x = round_px((float)child_x - inset);
        if (inset_px(child_spec->top, child_spec->top_pct, content_height, &inset))
            child_y = round_px((float)child_y + inset);
        else if (inset_px(child_spec->bottom, child_spec->bottom_pct, content_height, &inset))
            child_y = round_px((float)child_y - inset);

        child->computed.x = child_x;
        child->computed.y = child_y;
        child->computed.w = child_width;
        child->computed.h = child_height;
    }

    /* Pass 6b — recurse via sibling chain using pre-stored computed rects. A display:none child was
     * excluded from every sizing pass above, so its computed rect still holds whatever it had when it
     * was last visible; run it through compute_layout anyway (which zeroes the box and stops without
     * recursing) so a hidden node reports the empty layout it actually occupies — onLayout included —
     * rather than a stale one. Absolutely-positioned children are otherwise handled by pass 7, but a
     * hidden one is collapsed here since pass 7 skips it too. */
    for (uint16_t child_tag = node->first_child_tag; child_tag != ER_INVALID_TAG;)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        if (child->layout.display == ER_DISPLAY_NONE)
            compute_layout(child->tag, 0, 0, 0, 0);
        else if (child->layout.position != ER_POS_ABSOLUTE)
            compute_layout(child->tag, child->computed.w, child->computed.h, child->computed.x, child->computed.y);
        child_tag = child->next_sibling_tag;
    }

    /*--------------------------------------------------------------------------
     * Pass 7 — absolutely positioned children.
     *------------------------------------------------------------------------*/
    for (uint16_t child_tag = node->first_child_tag; child_tag != ER_INVALID_TAG;)
    {
        ERNode* child = er_get_node(child_tag);
        if (!child)
            break;
        if (child->layout.position == ER_POS_ABSOLUTE && child->layout.display != ER_DISPLAY_NONE)
        {
            const ERLayoutSpec* child_spec = &child->layout;
            const int16_t margin_left = edge_or(child_spec->margin_left, child_spec->margin);
            const int16_t margin_right = edge_or(child_spec->margin_right, child_spec->margin);
            const int16_t margin_top = edge_or(child_spec->margin_top, child_spec->margin);
            const int16_t margin_bottom = edge_or(child_spec->margin_bottom, child_spec->margin);

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
            float inset_left = 0.0f, inset_right = 0.0f, inset_top = 0.0f, inset_bottom = 0.0f;
            const bool has_left = inset_px(child_spec->left, child_spec->left_pct, width, &inset_left);
            const bool has_right = inset_px(child_spec->right, child_spec->right_pct, width, &inset_right);
            const bool has_top = inset_px(child_spec->top, child_spec->top_pct, height, &inset_top);
            const bool has_bottom = inset_px(child_spec->bottom, child_spec->bottom_pct, height, &inset_bottom);

            float child_width = 0.0f, child_height = 0.0f;
            bool have_width = true, have_height = true;
            if (child_spec->width != ER_LAYOUT_AUTO)
                child_width = (float)child_spec->width;
            else if (child_spec->width_pct > 0.0f)
                child_width = (float)width * child_spec->width_pct / PERCENT_SCALE;
            else if (has_left && has_right)
                child_width = (float)(width - margin_left - margin_right) - inset_left - inset_right;
            else
                have_width = false;

            if (child_spec->height != ER_LAYOUT_AUTO)
                child_height = (float)child_spec->height;
            else if (child_spec->height_pct > 0.0f)
                child_height = (float)height * child_spec->height_pct / PERCENT_SCALE;
            else if (has_top && has_bottom)
                child_height = (float)(height - margin_top - margin_bottom) - inset_top - inset_bottom;
            else
                have_height = false;

            /* aspect_ratio (= width / height) fills an axis from the one already resolved, exactly as
             * Pass 1 derives a flow child's cross size from its main. */
            if (child_spec->aspect_ratio > 0.0f && have_width != have_height)
            {
                if (have_width)
                {
                    child_height = child_width / child_spec->aspect_ratio;
                    have_height = true;
                }
                else
                {
                    child_width = child_height * child_spec->aspect_ratio;
                    have_width = true;
                }
            }

            /* Still unresolved: size to content, the same max-content measurement a flow child falls
             * back on, instead of collapsing to 0. A zero-sized box paints its children fine (nothing
             * clips them), so the collapse was invisible on screen but fatal to input: hit_test_node
             * gates entry on the node's own rect, so an empty one turned the whole subtree into a dead
             * zone and let every touch fall through to what was behind it. measure_content
             * is memoised per layout pass, so this costs one measurement per absolute node. */
            if (!have_width || !have_height)
            {
                int16_t intrinsic_width = 0, intrinsic_height = 0;
                measure_content(child_tag,
                                have_width ? round_px(child_width) : ER_LAYOUT_AUTO,
                                &intrinsic_width,
                                &intrinsic_height);
                if (!have_width)
                    child_width = (float)intrinsic_width;
                if (!have_height)
                    child_height = (float)intrinsic_height;
            }

            child_width = clamp_sizef(child_width, child_spec->min_width, child_spec->max_width);
            child_height = clamp_sizef(child_height, child_spec->min_height, child_spec->max_height);

            /* An axis with NO inset falls back to its static position: where this child would have sat
             * had it stayed in flow, which is inside the CONTENT box and honours the parent's
             * justifyContent / alignItems (the child's own alignSelf winning, as in Pass 1). Resolved
             * per axis, so `left: 10` with no `top` pins x and still aligns y. */
            uint8_t self_align = (child_spec->align_self != ER_ALIGN_AUTO) ? child_spec->align_self : spec->align_items;
            if (self_align == ER_ALIGN_AUTO)
                self_align = ER_ALIGN_STRETCH;

            /* The static position places a whole node, so it works off the rounded size. */
            const int16_t rounded_width = round_px(child_width);
            const int16_t rounded_height = round_px(child_height);
            const int16_t solo_main_size = is_row ? rounded_width : rounded_height;
            const int16_t solo_cross_size = is_row ? rounded_height : rounded_width;
            const int16_t main_margin_lead = is_row ? margin_left : margin_top;
            const int16_t main_margin_trail = is_row ? margin_right : margin_bottom;
            const int16_t cross_margin_lead = is_row ? margin_top : margin_left;
            const int16_t cross_margin_trail = is_row ? margin_bottom : margin_right;

            const int16_t main_static = solo_axis_pos(
                is_row ? content_x : content_y,
                main_size,
                solo_main_size,
                main_margin_lead,
                main_margin_trail,
                solo_main_offset2(spec->justify_content,
                                  (int16_t)(main_size - solo_main_size - main_margin_lead - main_margin_trail)),
                is_reverse_dir(spec->flex_direction));
            const int16_t cross_static = solo_axis_pos(
                is_row ? content_y : content_x,
                cross_avail,
                solo_cross_size,
                cross_margin_lead,
                cross_margin_trail,
                solo_cross_offset2(self_align,
                                   (int16_t)(cross_avail - solo_cross_size - cross_margin_lead - cross_margin_trail)),
                spec->flex_wrap == ER_WRAP_WRAP_REVERSE);

            /* Insets measure from the padding box; an axis pinned by neither takes the static position. */
            float edge_x, edge_y;
            if (has_left)
                edge_x = (float)(x + margin_left) + inset_left;
            else if (has_right)
                edge_x = (float)(x + width - margin_right) - inset_right - child_width;
            else
                edge_x = (float)(is_row ? main_static : cross_static);

            if (has_top)
                edge_y = (float)(y + margin_top) + inset_top;
            else if (has_bottom)
                edge_y = (float)(y + height - margin_bottom) - inset_bottom - child_height;
            else
                edge_y = (float)(is_row ? cross_static : main_static);

            /* Both edges land on the pixel grid and the size is the distance between them, so a box
             * straddling a half pixel keeps it instead of losing it to a rounded-down size. */
            const int16_t child_x = round_px(edge_x);
            const int16_t child_y = round_px(edge_y);
            compute_layout(child_tag,
                           span_px(child_x, round_px(edge_x + child_width)),
                           span_px(child_y, round_px(edge_y + child_height)),
                           child_x,
                           child_y);
        }
        child_tag = child->next_sibling_tag;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_layout_compute(uint16_t root_tag, int16_t width, int16_t height)
{
    /* Advance the measure-cache generation so every measure_content() call this pass
     * starts with a clean slate. gen 0 is reserved as "never valid" (a fresh
     * s_measure_cache is zero-initialized), so skip it on wraparound; a wrap also means
     * every table slot could coincidentally hold stale gen 1 already, so clear it. */
    s_measure_generation++;
    if (s_measure_generation == 0U)
    {
        memset(s_measure_cache, 0, sizeof(s_measure_cache));
        s_measure_generation = 1U;
    }
    compute_layout(root_tag, width, height, 0, 0);
}
