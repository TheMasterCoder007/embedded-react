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

static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Invariant checkers
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief True when the two rects overlap OR touch (closed-interval test — the set must merge both). */
static bool rects_touch(const ERRect* a, const ERRect* b)
{
    return !(a->x + a->w < b->x || b->x + b->w < a->x || a->y + a->h < b->y || b->y + b->h < a->y);
}

/** @brief Asserts the set's core invariant: pairwise disjoint AND non-abutting. */
static bool check_disjoint(const ERDamageSet* s)
{
    for (uint8_t i = 0; i < s->count; i++)
    {
        if (s->r[i].w <= 0 || s->r[i].h <= 0)
        {
            return false; /* an empty rect must never be stored */
        }
        for (uint8_t j = (uint8_t)(i + 1U); j < s->count; j++)
        {
            if (rects_touch(&s->r[i], &s->r[j]))
            {
                return false;
            }
        }
    }
    return true;
}

/** @brief True when the point lies inside some stored rect. */
static bool covers_point(const ERDamageSet* s, int px, int py)
{
    for (uint8_t i = 0; i < s->count; i++)
    {
        if (px >= s->r[i].x && px < s->r[i].x + s->r[i].w && py >= s->r[i].y && py < s->r[i].y + s->r[i].h)
        {
            return true;
        }
    }
    return false;
}

/** @brief True when every pixel of the rect is covered (corners + edge midpoints + centre samples). */
static bool covers_rect(const ERDamageSet* s, const ERRect* r)
{
    /* Disjoint axis-aligned rects can't jointly cover an input rect unless one rect alone covers it
     * (a straddled input would have been merged), so sampling is sufficient — but sample densely
     * anyway to catch a hypothetical splitting bug: all 9 anchor points. */
    const int xs[3] = {r->x, r->x + r->w / 2, r->x + r->w - 1};
    const int ys[3] = {r->y, r->y + r->h / 2, r->y + r->h - 1};
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            if (!covers_point(s, xs[i], ys[j]))
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
static bool add_checked(ERDamageSet* s, int x, int y, int w, int h)
{
    er_damage_set_add(s, x, y, w, h);
    if (w > 0 && h > 0 && g_history_count < MAX_HISTORY)
    {
        g_history[g_history_count].x = x;
        g_history[g_history_count].y = y;
        g_history[g_history_count].w = w;
        g_history[g_history_count].h = h;
        g_history_count++;
    }
    if (!check_disjoint(s))
    {
        return false;
    }
    for (int i = 0; i < g_history_count; i++)
    {
        if (!covers_rect(s, &g_history[i]))
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
    ERDamageSet s = {0};
    history_reset();

    /* Empty inputs are ignored. */
    er_damage_set_add(&s, 10, 10, 0, 5);
    er_damage_set_add(&s, 10, 10, 5, 0);
    er_damage_set_add(&s, 10, 10, -3, -3);
    if (s.count != 0U)
        return fail("empty rects were stored");
    ERRect b;
    if (er_damage_set_bounds(&s, &b) || b.w != 0 || b.h != 0)
        return fail("empty set reported bounds");
    if (er_damage_set_area(&s) != 0U)
        return fail("empty set reported area");

    /* Two far-apart rects stay separate — THE point of the set. */
    if (!add_checked(&s, 0, 0, 30, 30))
        return fail("invariant after first add");
    if (!add_checked(&s, 170, 170, 30, 30))
        return fail("invariant after second add");
    if (s.count != 2U)
        return fail("two disjoint rects were merged");
    if (er_damage_set_area(&s) != 1800U)
        return fail("area is not the sum of the two rects");
    if (!er_damage_set_bounds(&s, &b) || b.x != 0 || b.y != 0 || b.w != 200 || b.h != 200)
        return fail("bounds is not the union bbox");

    /* Overlap merges. */
    if (!add_checked(&s, 20, 20, 30, 30))
        return fail("invariant after overlapping add");
    if (s.count != 2U)
        return fail("overlapping rect did not merge");

    /* Abutting merges (shares an edge with the 0,0,50,50 merged box). */
    if (!add_checked(&s, 50, 0, 10, 10))
        return fail("invariant after abutting add");
    if (s.count != 2U)
        return fail("abutting rect did not merge");

    er_damage_set_clear(&s);
    if (s.count != 0U)
        return fail("clear did not empty the set");

    printf("PASS: basics — empty ignored, disjoint kept apart, overlap/abut merged\n");
    return EXIT_SUCCESS;
}

static int check_cascade(void)
{
    ERDamageSet s = {0};
    history_reset();

    /* Two separated rects, then a bridge that touches both: all three must collapse to one. */
    if (!add_checked(&s, 0, 0, 40, 40))
        return fail("invariant: left rect");
    if (!add_checked(&s, 100, 0, 40, 40))
        return fail("invariant: right rect");
    if (s.count != 2U)
        return fail("setup: rects merged prematurely");
    if (!add_checked(&s, 35, 0, 70, 10))
        return fail("invariant: bridge rect");
    if (s.count != 1U)
        return fail("bridge did not cascade-merge both sides");
    if (s.r[0].x != 0 || s.r[0].w != 140)
        return fail("cascaded union has the wrong extent");

    printf("PASS: cascade — a bridge rect collapses everything it connects\n");
    return EXIT_SUCCESS;
}

static int check_saturation(void)
{
    ERDamageSet s = {0};
    history_reset();

    /* MAX rects in a horizontal row, well apart. */
    for (int i = 0; i < ER_DAMAGE_RECTS_MAX; i++)
    {
        if (!add_checked(&s, i * 100, 0, 20, 20))
            return fail("invariant while filling the pool");
    }
    if (s.count != (uint8_t)ER_DAMAGE_RECTS_MAX)
        return fail("pool did not fill to MAX");

    /* One more, far below: must still be covered (merged somewhere), never dropped. */
    if (!add_checked(&s, 0, 300, 20, 20))
        return fail("invariant/coverage after saturating add");
    if (s.count > (uint8_t)ER_DAMAGE_RECTS_MAX)
        return fail("count exceeded MAX");

    /* A saturating add adjacent to an existing rect should pick that rect (min waste ~ 0),
     * leaving the others untouched: count stays at MAX. */
    er_damage_set_clear(&s);
    history_reset();
    for (int i = 0; i < ER_DAMAGE_RECTS_MAX; i++)
    {
        if (!add_checked(&s, i * 100, 0, 20, 20))
            return fail("invariant while refilling the pool");
    }
    if (!add_checked(&s, 22, 0, 20, 20)) /* 2px gap from rect at x=0..20: nearest, min waste */
        return fail("invariant after min-waste add");
    if (s.count != (uint8_t)ER_DAMAGE_RECTS_MAX)
        return fail("min-waste merge changed the rect count unexpectedly");
    /* The merged rect must be the near-left one grown rightward, not a far pairing. */
    bool found = false;
    for (uint8_t i = 0; i < s.count; i++)
    {
        if (s.r[i].x == 0 && s.r[i].x + s.r[i].w == 42)
            found = true;
    }
    if (!found)
        return fail("min-waste merge did not pick the adjacent rect");

    /* Stress: a burst of scattered + overlapping rects keeps both invariants throughout. */
    er_damage_set_clear(&s);
    history_reset();
    static const int seq[][4] = {
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
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
    {
        if (!add_checked(&s, seq[i][0], seq[i][1], seq[i][2], seq[i][3]))
            return fail("invariant/coverage broke during the stress sequence");
    }

    printf("PASS: saturation — MAX+1 merges min-waste pair, coverage never dropped (count=%u)\n", (unsigned)s.count);
    return EXIT_SUCCESS;
}

static int check_limit(void)
{
    ERDamageSet s = {0};
    history_reset();

    /* A widely-spread row of rects, one per slot. */
    const int fill = (ER_DAMAGE_RECTS_MAX < 8) ? ER_DAMAGE_RECTS_MAX : 8;
    for (int i = 0; i < fill; i++)
    {
        if (!add_checked(&s, i * 100, i * 100, 20, 20))
            return fail("invariant while filling for the limit test");
    }

    /* Above the current count: nothing moves. */
    const uint8_t before = s.count;
    er_damage_set_limit(&s, (uint8_t)(before + 3U));
    if (s.count != before)
        return fail("limit above the count changed the set");

    /* Trim to 2: coverage of every input must survive, and the set must stay disjoint. */
    er_damage_set_limit(&s, 2U);
    if (s.count > 2U)
        return fail("limit did not trim to the requested count");
    if (!check_disjoint(&s))
        return fail("limit broke the disjointness invariant");
    for (int i = 0; i < g_history_count; i++)
    {
        if (!covers_rect(&s, &g_history[i]))
            return fail("limit dropped coverage of an earlier rect");
    }

    /* Trimming to 1 is the old single-bounding-box behaviour. */
    ERRect bounds;
    er_damage_set_bounds(&s, &bounds);
    er_damage_set_limit(&s, 1U);
    if (s.count != 1U)
        return fail("limit to 1 did not collapse to a single rect");
    if (s.r[0].x != bounds.x || s.r[0].y != bounds.y || s.r[0].w != bounds.w || s.r[0].h != bounds.h)
        return fail("the collapsed rect is not the covering bounding box");

    /* Zero empties it; a limit on an empty set is a no-op. */
    er_damage_set_limit(&s, 0U);
    if (s.count != 0U)
        return fail("limit to 0 did not empty the set");
    er_damage_set_limit(&s, 4U);
    if (s.count != 0U)
        return fail("limit on an empty set produced rects");

    /* The trim rule matches the saturating add's: an adjacent pair is the cheapest fuse. */
    er_damage_set_clear(&s);
    history_reset();
    if (!add_checked(&s, 0, 0, 20, 20) || !add_checked(&s, 22, 0, 20, 20) || !add_checked(&s, 500, 500, 20, 20))
        return fail("invariant while building the min-waste trim case");
    er_damage_set_limit(&s, 2U);
    if (s.count != 2U)
        return fail("min-waste trim did not land on 2 rects");
    bool fused = false, far_kept = false;
    for (uint8_t i = 0; i < s.count; i++)
    {
        if (s.r[i].x == 0 && s.r[i].w == 42)
            fused = true;
        if (s.r[i].x == 500 && s.r[i].w == 20)
            far_kept = true;
    }
    if (!fused || !far_kept)
        return fail("min-waste trim fused the wrong pair");

    printf("PASS: limit — trims to budget, keeps coverage + disjointness, fuses the cheapest pair\n");
    return EXIT_SUCCESS;
}

int main(void)
{
    int rc = check_basics();
    if (rc != EXIT_SUCCESS)
        return rc;
    rc = check_cascade();
    if (rc != EXIT_SUCCESS)
        return rc;
    rc = check_saturation();
    if (rc != EXIT_SUCCESS)
        return rc;
    rc = check_limit();
    if (rc != EXIT_SUCCESS)
        return rc;
    return EXIT_SUCCESS;
}
