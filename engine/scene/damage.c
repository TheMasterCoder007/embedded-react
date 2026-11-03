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

#include "er_damage_internal.h"

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief True when the two rects overlap OR abut (share an edge or a corner).
 *
 * Closed-interval test on [x, x+w] rather than [x, x+w): touching rects painted as separate clipped
 * passes would be pixel-correct, but merging them saves a pass, so "touching counts" is deliberate.
 */
static bool touch_or_overlap(const ERRect* first, const ERRect* second)
{
    return !(first->x + first->w < second->x || second->x + second->w < first->x || first->y + first->h < second->y
             || second->y + second->h < first->y);
}

/** @brief Grows @p target in place to the bounding box of target ∪ other. */
static void bbox_union(ERRect* target, const ERRect* other)
{
    const int right = (target->x + target->w > other->x + other->w) ? target->x + target->w : other->x + other->w;
    const int bottom = (target->y + target->h > other->y + other->h) ? target->y + target->h : other->y + other->h;
    if (other->x < target->x)
        target->x = other->x;
    if (other->y < target->y)
        target->y = other->y;
    target->w = right - target->x;
    target->h = bottom - target->y;
}

/** @brief Rect area in pixels (fields are non-negative by construction). */
static uint32_t rect_area(const ERRect* rect)
{
    return (uint32_t)rect->w * (uint32_t)rect->h;
}

/**
 * @brief Absorbs into @p accumulated every stored rect that overlaps or abuts it, cascading until stable.
 *
 * Each absorbed rect is swap-removed and its box unioned into @p accumulated; because the union can grow
 * @p accumulated into contact with rects already checked, the scan restarts whenever anything was absorbed.
 * Terminates: every restart is paid for by count strictly decreasing.
 */
static void absorb_touching(ERDamageSet* set, ERRect* accumulated)
{
    bool removed_any = true;
    while (removed_any)
    {
        removed_any = false;
        for (uint8_t rect_index = 0; rect_index < set->count;)
        {
            if (touch_or_overlap(accumulated, &set->rects[rect_index]))
            {
                bbox_union(accumulated, &set->rects[rect_index]);
                set->rects[rect_index] =
                    set->rects[--set->count]; /* swap-remove; re-test the swapped-in rect at rect_index */
                removed_any = true;
            }
            else
            {
                rect_index++;
            }
        }
    }
}

/**
 * @brief Index of the stored rect whose bbox union with @p accumulated drags in the fewest clean pixels.
 *
 * @p accumulated must be disjoint from every stored rect (run absorb_touching first), so
 * waste = area(union) − area(accumulated) − area(rect) is exactly the untouched area the merge would repaint.
 * The rect stays in the set; unioning it into @p accumulated and re-absorbing is what removes it. Requires a
 * non-empty set.
 */
static uint8_t least_waste_partner(const ERDamageSet* set, const ERRect* accumulated)
{
    uint8_t best_index = 0U;
    uint32_t best_waste = UINT32_MAX;
    for (uint8_t rect_index = 0; rect_index < set->count; rect_index++)
    {
        ERRect merged = *accumulated;
        bbox_union(&merged, &set->rects[rect_index]);
        const uint32_t waste = rect_area(&merged) - rect_area(accumulated) - rect_area(&set->rects[rect_index]);
        if (waste < best_waste)
        {
            best_waste = waste;
            best_index = rect_index;
        }
    }
    return best_index;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (engine-internal)
 ---------------------------------------------------------------------------------------------------------------------*/

void er_damage_set_clear(ERDamageSet* set)
{
    set->count = 0U;
}

void er_damage_set_add(ERDamageSet* set, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    ERRect accumulated;
    accumulated.x = x;
    accumulated.y = y;
    accumulated.w = width;
    accumulated.h = height;

    /* Merge with everything the input touches (disjointness invariant), cascading. */
    absorb_touching(set, &accumulated);

    if (set->count < (uint8_t)ER_DAMAGE_RECTS_MAX)
    {
        set->rects[set->count++] = accumulated;
        return;
    }

    /* Saturated: merge with the stored rect that wastes the least area. `accumulated` is disjoint from every
     * stored rect here (absorb_touching just ran), so waste = union − accumulated − rect is >= 0 and measures
     * exactly the clean pixels the merge drags into the repaint. The second absorb then swallows that
     * partner (accumulated now contains it) plus anything the grown box newly touches, so count drops by at
     * least one and the final append always fits. */
    bbox_union(&accumulated, &set->rects[least_waste_partner(set, &accumulated)]);
    absorb_touching(set, &accumulated);
    set->rects[set->count++] = accumulated;
}

void er_damage_set_limit(ERDamageSet* set, uint8_t max_rects)
{
    if (max_rects == 0U)
    {
        set->count = 0U;
        return;
    }

    /* Repeatedly fuse the globally cheapest PAIR until the set fits — the same waste measure the
     * saturating add uses, but over every pair rather than one fixed incoming rect, because here no
     * rect is more "incoming" than another and fusing an arbitrary one would happily pair two far
     * corners. The union swallows both members, so absorb_touching removes them (plus anything the
     * grown box newly touches) and the append nets at least one rect gone — hence this terminates. */
    while (set->count > max_rects)
    {
        uint8_t best_first = 0U, best_second = 1U;
        uint32_t best_waste = UINT32_MAX;
        for (uint8_t rect_index = 0U; rect_index < set->count; rect_index++)
        {
            for (uint8_t other_index = (uint8_t)(rect_index + 1U); other_index < set->count; other_index++)
            {
                ERRect merged = set->rects[rect_index];
                bbox_union(&merged, &set->rects[other_index]);
                const uint32_t waste =
                    rect_area(&merged) - rect_area(&set->rects[rect_index]) - rect_area(&set->rects[other_index]);
                if (waste < best_waste)
                {
                    best_waste = waste;
                    best_first = rect_index;
                    best_second = other_index;
                }
            }
        }
        ERRect accumulated = set->rects[best_first];
        bbox_union(&accumulated, &set->rects[best_second]);
        absorb_touching(set, &accumulated);
        set->rects[set->count++] = accumulated;
    }
}

bool er_damage_set_bounds(const ERDamageSet* set, ERRect* out)
{
    if (set->count == 0U)
    {
        if (out)
        {
            out->x = 0;
            out->y = 0;
            out->w = 0;
            out->h = 0;
        }
        return false;
    }
    if (out)
    {
        *out = set->rects[0];
        for (uint8_t rect_index = 1U; rect_index < set->count; rect_index++)
        {
            bbox_union(out, &set->rects[rect_index]);
        }
    }
    return true;
}

uint32_t er_damage_set_area(const ERDamageSet* set)
{
    uint32_t total = 0U;
    for (uint8_t rect_index = 0U; rect_index < set->count; rect_index++)
    {
        total += rect_area(&set->rects[rect_index]);
    }
    return total;
}
