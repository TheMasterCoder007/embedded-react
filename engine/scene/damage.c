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
static bool touch_or_overlap(const ERRect* a, const ERRect* b)
{
    return !(a->x + a->w < b->x || b->x + b->w < a->x || a->y + a->h < b->y || b->y + b->h < a->y);
}

/** @brief Grows @p a in place to the bounding box of a ∪ b. */
static void bbox_union(ERRect* a, const ERRect* b)
{
    const int x1 = (a->x + a->w > b->x + b->w) ? a->x + a->w : b->x + b->w;
    const int y1 = (a->y + a->h > b->y + b->h) ? a->y + a->h : b->y + b->h;
    if (b->x < a->x)
        a->x = b->x;
    if (b->y < a->y)
        a->y = b->y;
    a->w = x1 - a->x;
    a->h = y1 - a->y;
}

/** @brief Rect area in pixels (fields are non-negative by construction). */
static uint32_t rect_area(const ERRect* r)
{
    return (uint32_t)r->w * (uint32_t)r->h;
}

/**
 * @brief Absorbs into @p acc every stored rect that overlaps or abuts it, cascading until stable.
 *
 * Each absorbed rect is swap-removed and its box unioned into @p acc; because the union can grow
 * @p acc into contact with rects already checked, the scan restarts whenever anything was absorbed.
 * Terminates: every restart is paid for by count strictly decreasing.
 */
static void absorb_touching(ERDamageSet* s, ERRect* acc)
{
    bool removed_any = true;
    while (removed_any)
    {
        removed_any = false;
        for (uint8_t i = 0; i < s->count;)
        {
            if (touch_or_overlap(acc, &s->r[i]))
            {
                bbox_union(acc, &s->r[i]);
                s->r[i] = s->r[--s->count]; /* swap-remove; re-test the swapped-in rect at i */
                removed_any = true;
            }
            else
            {
                i++;
            }
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (engine-internal)
 ---------------------------------------------------------------------------------------------------------------------*/

void er_damage_set_clear(ERDamageSet* s)
{
    s->count = 0U;
}

void er_damage_set_add(ERDamageSet* s, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }

    ERRect acc;
    acc.x = x;
    acc.y = y;
    acc.w = w;
    acc.h = h;

    /* Merge with everything the input touches (disjointness invariant), cascading. */
    absorb_touching(s, &acc);

    if (s->count < (uint8_t)ER_DAMAGE_RECTS_MAX)
    {
        s->r[s->count++] = acc;
        return;
    }

    /* Saturated: merge with the stored rect that wastes the least area. `acc` is disjoint from every
     * stored rect here (absorb_touching just ran), so waste = union − acc − rect is >= 0 and measures
     * exactly the clean pixels the merge drags into the repaint. The merged box can newly touch other
     * rects, so absorb again — that only shrinks count, guaranteeing the final append fits. */
    uint8_t best = 0U;
    uint32_t best_waste = UINT32_MAX;
    for (uint8_t i = 0; i < s->count; i++)
    {
        ERRect u = acc;
        bbox_union(&u, &s->r[i]);
        const uint32_t waste = rect_area(&u) - rect_area(&acc) - rect_area(&s->r[i]);
        if (waste < best_waste)
        {
            best_waste = waste;
            best = i;
        }
    }
    bbox_union(&acc, &s->r[best]);
    s->r[best] = s->r[--s->count];
    absorb_touching(s, &acc);
    s->r[s->count++] = acc;
}

bool er_damage_set_bounds(const ERDamageSet* s, ERRect* out)
{
    if (s->count == 0U)
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
        *out = s->r[0];
        for (uint8_t i = 1U; i < s->count; i++)
        {
            bbox_union(out, &s->r[i]);
        }
    }
    return true;
}

uint32_t er_damage_set_area(const ERDamageSet* s)
{
    uint32_t total = 0U;
    for (uint8_t i = 0U; i < s->count; i++)
    {
        total += rect_area(&s->r[i]);
    }
    return total;
}
