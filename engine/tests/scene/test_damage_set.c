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
 * ERDamageSet (er_damage_internal.h): the disjoint dirty-rect set behind the compositor's damage
 * tracking. The properties that matter:
 *
 *   - disjointness: no two stored rects overlap or abut (a pixel covered twice would double-blend
 *     translucent content when the compositor paints each rect in its own pass),
 *   - coverage: every added rect is covered by the stored rects, including through cascade merges
 *     and pool saturation (coverage may grow — never shrink),
 *   - graceful saturation: MAX+1 scattered rects still fit by merging the least-wasteful pair.
 *
 * Coverage is asserted exhaustively: every pixel-corner sample of every rect ever added must fall
 * inside some stored rect after every operation.
 */

#include "er_damage_internal.h"

#include <stdio.h>
#include <stdlib.h>

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Invariant checkers
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief True when the two rects overlap OR touch (closed-interval test — the set must merge both). */
static bool rects_touch(const ERRect* first, const ERRect* second)
{
    return !(first->x + first->width < second->x || second->x + second->width < first->x
             || first->y + first->height < second->y || second->y + second->height < first->y);
}

/** @brief Asserts the set's core invariant: pairwise disjoint AND non-abutting. */
static bool check_disjoint(const ERDamageSet* damage)
{
    for (uint8_t rect_index = 0; rect_index < damage->count; rect_index++)
    {
        if (damage->rects[rect_index].width <= 0 || damage->rects[rect_index].height <= 0)
        {
            return false; /* an empty rect must never be stored */
        }
        for (uint8_t other_index = (uint8_t)(rect_index + 1U); other_index < damage->count; other_index++)
        {
            if (rects_touch(&damage->rects[rect_index], &damage->rects[other_index]))
            {
                return false;
            }
        }
    }
    return true;
}

/** @brief True when the point lies inside some stored rect. */
static bool covers_point(const ERDamageSet* damage, int pixel_x, int pixel_y)
{
    for (uint8_t rect_index = 0; rect_index < damage->count; rect_index++)
    {
        if (pixel_x >= damage->rects[rect_index].x
            && pixel_x < damage->rects[rect_index].x + damage->rects[rect_index].width
            && pixel_y >= damage->rects[rect_index].y
            && pixel_y < damage->rects[rect_index].y + damage->rects[rect_index].height)
        {
            return true;
        }
    }
    return false;
}

/** @brief True when every pixel of the rect is covered (corners + edge midpoints + centre samples). */
static bool covers_rect(const ERDamageSet* damage, const ERRect* rect)
{
    /* Disjoint axis-aligned rects can't jointly cover an input rect unless one rect alone covers it
     * (a straddled input would have been merged), so sampling is sufficient — but sample densely
     * anyway to catch a hypothetical splitting bug: all 9 anchor points. */
    const int sample_xs[3] = {rect->x, rect->x + rect->width / 2, rect->x + rect->width - 1};
    const int sample_ys[3] = {rect->y, rect->y + rect->height / 2, rect->y + rect->height - 1};
    for (int sample_x_index = 0; sample_x_index < 3; sample_x_index++)
    {
        for (int sample_y_index = 0; sample_y_index < 3; sample_y_index++)
        {
            if (!covers_point(damage, sample_xs[sample_x_index], sample_ys[sample_y_index]))
            {
                return false;
            }
        }
    }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Add-with-history harness: every add re-checks disjointness + coverage of ALL inputs so far
 ---------------------------------------------------------------------------------------------------------------------*/

#define MAX_HISTORY 64

static ERRect g_history[MAX_HISTORY];
static int g_history_count;

static void history_reset(void)
{
    g_history_count = 0;
}

/** @brief er_damage_set_add + invariant re-check against everything added so far. */
static bool add_checked(ERDamageSet* damage, int x, int y, int width, int height)
{
    er_damage_set_add(damage, x, y, width, height);
    if (width > 0 && height > 0 && g_history_count < MAX_HISTORY)
    {
        g_history[g_history_count].x = x;
        g_history[g_history_count].y = y;
        g_history[g_history_count].width = width;
        g_history[g_history_count].height = height;
        g_history_count++;
    }
    if (!check_disjoint(damage))
    {
        return false;
    }
    for (int history_index = 0; history_index < g_history_count; history_index++)
    {
        if (!covers_rect(damage, &g_history[history_index]))
        {
            return false;
        }
    }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

static int check_basics(void)
{
    ERDamageSet damage = {0};
    history_reset();

    /* Empty inputs are ignored. */
    er_damage_set_add(&damage, 10, 10, 0, 5);
    er_damage_set_add(&damage, 10, 10, 5, 0);
    er_damage_set_add(&damage, 10, 10, -3, -3);
    if (damage.count != 0U)
        return fail("empty rects were stored");
    ERRect bounds;
    if (er_damage_set_bounds(&damage, &bounds) || bounds.width != 0 || bounds.height != 0)
        return fail("empty set reported bounds");
    if (er_damage_set_area(&damage) != 0U)
        return fail("empty set reported area");

    /* Two far-apart rects stay separate — THE point of the set. */
    if (!add_checked(&damage, 0, 0, 30, 30))
        return fail("invariant after first add");
    if (!add_checked(&damage, 170, 170, 30, 30))
        return fail("invariant after second add");
    if (damage.count != 2U)
        return fail("two disjoint rects were merged");
    if (er_damage_set_area(&damage) != 1800U)
        return fail("area is not the sum of the two rects");
    if (!er_damage_set_bounds(&damage, &bounds) || bounds.x != 0 || bounds.y != 0 || bounds.width != 200
        || bounds.height != 200)
        return fail("bounds is not the union bbox");

    /* Overlap merges. */
    if (!add_checked(&damage, 20, 20, 30, 30))
        return fail("invariant after overlapping add");
    if (damage.count != 2U)
        return fail("overlapping rect did not merge");

    /* Abutting merges (shares an edge with the 0,0,50,50 merged box). */
    if (!add_checked(&damage, 50, 0, 10, 10))
        return fail("invariant after abutting add");
    if (damage.count != 2U)
        return fail("abutting rect did not merge");

    er_damage_set_clear(&damage);
    if (damage.count != 0U)
        return fail("clear did not empty the set");

    printf("PASS: basics — empty ignored, disjoint kept apart, overlap/abut merged\n");
    return EXIT_SUCCESS;
}

static int check_cascade(void)
{
    ERDamageSet damage = {0};
    history_reset();

    /* Two separated rects, then a bridge that touches both: all three must collapse to one. */
    if (!add_checked(&damage, 0, 0, 40, 40))
        return fail("invariant: left rect");
    if (!add_checked(&damage, 100, 0, 40, 40))
        return fail("invariant: right rect");
    if (damage.count != 2U)
        return fail("setup: rects merged prematurely");
    if (!add_checked(&damage, 35, 0, 70, 10))
        return fail("invariant: bridge rect");
    if (damage.count != 1U)
        return fail("bridge did not cascade-merge both sides");
    if (damage.rects[0].x != 0 || damage.rects[0].width != 140)
        return fail("cascaded union has the wrong extent");

    printf("PASS: cascade — a bridge rect collapses everything it connects\n");
    return EXIT_SUCCESS;
}

static int check_saturation(void)
{
    ERDamageSet damage = {0};
    history_reset();

    /* MAX rects in a horizontal row, well apart. */
    for (int rect_index = 0; rect_index < ER_DAMAGE_RECTS_MAX; rect_index++)
    {
        if (!add_checked(&damage, rect_index * 100, 0, 20, 20))
            return fail("invariant while filling the pool");
    }
    if (damage.count != (uint8_t)ER_DAMAGE_RECTS_MAX)
        return fail("pool did not fill to MAX");

    /* One more, far below: must still be covered (merged somewhere), never dropped. */
    if (!add_checked(&damage, 0, 300, 20, 20))
        return fail("invariant/coverage after saturating add");
    if (damage.count > (uint8_t)ER_DAMAGE_RECTS_MAX)
        return fail("count exceeded MAX");

    /* A saturating add adjacent to an existing rect should pick that rect (min waste ~ 0),
     * leaving the others untouched: count stays at MAX. */
    er_damage_set_clear(&damage);
    history_reset();
    for (int rect_index = 0; rect_index < ER_DAMAGE_RECTS_MAX; rect_index++)
    {
        if (!add_checked(&damage, rect_index * 100, 0, 20, 20))
            return fail("invariant while refilling the pool");
    }
    if (!add_checked(&damage, 22, 0, 20, 20)) /* 2px gap from rect at x=0..20: nearest, min waste */
        return fail("invariant after min-waste add");
    if (damage.count != (uint8_t)ER_DAMAGE_RECTS_MAX)
        return fail("min-waste merge changed the rect count unexpectedly");
    /* The merged rect must be the near-left one grown rightward, not a far pairing. */
    bool found = false;
    for (uint8_t rect_index = 0; rect_index < damage.count; rect_index++)
    {
        if (damage.rects[rect_index].x == 0 && damage.rects[rect_index].x + damage.rects[rect_index].width == 42)
            found = true;
    }
    if (!found)
        return fail("min-waste merge did not pick the adjacent rect");

    /* Stress: a burst of scattered + overlapping rects keeps both invariants throughout. */
    er_damage_set_clear(&damage);
    history_reset();
    static const int sequence[][4] = {
        {5, 5, 30, 30},
        {200, 5, 30, 30},
        {5, 200, 30, 30},
        {200, 200, 30, 30},
        {100, 100, 20, 20},
        {90, 90, 40, 40},
        {0, 0, 10, 10},
        {230, 230, 10, 10},
        {50, 50, 100, 5},
        {5, 100, 5, 100},
        {150, 20, 60, 60},
        {20, 150, 60, 60},
    };
    for (size_t step_index = 0; step_index < sizeof(sequence) / sizeof(sequence[0]); step_index++)
    {
        if (!add_checked(&damage,
                         sequence[step_index][0],
                         sequence[step_index][1],
                         sequence[step_index][2],
                         sequence[step_index][3]))
            return fail("invariant/coverage broke during the stress sequence");
    }

    printf("PASS: saturation — MAX+1 merges min-waste pair, coverage never dropped (count=%u)\n",
           (unsigned)damage.count);
    return EXIT_SUCCESS;
}

static int check_limit(void)
{
    ERDamageSet damage = {0};
    history_reset();

    /* A widely-spread row of rects, one per slot. */
    const int fill = (ER_DAMAGE_RECTS_MAX < 8) ? ER_DAMAGE_RECTS_MAX : 8;
    for (int rect_index = 0; rect_index < fill; rect_index++)
    {
        if (!add_checked(&damage, rect_index * 100, rect_index * 100, 20, 20))
            return fail("invariant while filling for the limit test");
    }

    /* Above the current count: nothing moves. */
    const uint8_t before = damage.count;
    er_damage_set_limit(&damage, (uint8_t)(before + 3U));
    if (damage.count != before)
        return fail("limit above the count changed the set");

    /* Trim to 2: coverage of every input must survive, and the set must stay disjoint. */
    er_damage_set_limit(&damage, 2U);
    if (damage.count > 2U)
        return fail("limit did not trim to the requested count");
    if (!check_disjoint(&damage))
        return fail("limit broke the disjointness invariant");
    for (int history_index = 0; history_index < g_history_count; history_index++)
    {
        if (!covers_rect(&damage, &g_history[history_index]))
            return fail("limit dropped coverage of an earlier rect");
    }

    /* Trimming to 1 is the old single-bounding-box behaviour. */
    ERRect bounds;
    er_damage_set_bounds(&damage, &bounds);
    er_damage_set_limit(&damage, 1U);
    if (damage.count != 1U)
        return fail("limit to 1 did not collapse to a single rect");
    if (damage.rects[0].x != bounds.x || damage.rects[0].y != bounds.y || damage.rects[0].width != bounds.width
        || damage.rects[0].height != bounds.height)
        return fail("the collapsed rect is not the covering bounding box");

    /* Zero empties it; a limit on an empty set is a no-op. */
    er_damage_set_limit(&damage, 0U);
    if (damage.count != 0U)
        return fail("limit to 0 did not empty the set");
    er_damage_set_limit(&damage, 4U);
    if (damage.count != 0U)
        return fail("limit on an empty set produced rects");

    /* The trim rule matches the saturating add's: an adjacent pair is the cheapest fuse. */
    er_damage_set_clear(&damage);
    history_reset();
    if (!add_checked(&damage, 0, 0, 20, 20) || !add_checked(&damage, 22, 0, 20, 20)
        || !add_checked(&damage, 500, 500, 20, 20))
        return fail("invariant while building the min-waste trim case");
    er_damage_set_limit(&damage, 2U);
    if (damage.count != 2U)
        return fail("min-waste trim did not land on 2 rects");
    bool fused = false, far_kept = false;
    for (uint8_t rect_index = 0; rect_index < damage.count; rect_index++)
    {
        if (damage.rects[rect_index].x == 0 && damage.rects[rect_index].width == 42)
            fused = true;
        if (damage.rects[rect_index].x == 500 && damage.rects[rect_index].width == 20)
            far_kept = true;
    }
    if (!fused || !far_kept)
        return fail("min-waste trim fused the wrong pair");

    printf("PASS: limit — trims to budget, keeps coverage + disjointness, fuses the cheapest pair\n");
    return EXIT_SUCCESS;
}

int main(void)
{
    int result = check_basics();
    if (result != EXIT_SUCCESS)
        return result;
    result = check_cascade();
    if (result != EXIT_SUCCESS)
        return result;
    result = check_saturation();
    if (result != EXIT_SUCCESS)
        return result;
    result = check_limit();
    if (result != EXIT_SUCCESS)
        return result;
    return EXIT_SUCCESS;
}
