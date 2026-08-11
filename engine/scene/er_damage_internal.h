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

#ifndef EMBEDDED_REACT_ER_DAMAGE_INTERNAL_H
#define EMBEDDED_REACT_ER_DAMAGE_INTERNAL_H

/*
 * Disjoint dirty-rect set — the engine's damage accumulator (private to the engine; used by the
 * compositor's pre-pass, the multi-buffer debt slots, and the removed-node damage).
 *
 * Historically every damage source was unioned into ONE bounding box, so a change in the top-left
 * corner plus one in the bottom-right repainted the whole span between them. This set keeps up to
 * ER_DAMAGE_RECTS_MAX rects instead, letting the compositor repaint (and a backend flush) each
 * changed area on its own.
 *
 * Invariant: the stored rects are PAIRWISE DISJOINT and non-abutting. This is load-bearing, not an
 * optimisation — the compositor paints each rect in a separate clipped pass, and a pixel covered by
 * two passes would have translucent content blended into it twice (visibly darker). er_damage_set_add
 * therefore merges any overlapping-or-touching input into the existing rects (cascading, since a
 * merge can grow a rect into contact with another) rather than storing it alongside them.
 *
 * When the set is full, the incoming rect is merged with whichever stored rect wastes the least area
 * (union area minus the two parts) — so pathological scattered damage degrades gracefully toward the
 * old single-bbox behaviour, and coverage is never dropped.
 */

#include "er_scene.h" /* ERRect */

#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/* ER_DAMAGE_RECTS_MAX (the per-set rect budget) is public — defined in er_scene.h next to
 * er_get_dirty_rects(), whose contract it caps. */

/**
 * @brief A small set of pairwise-disjoint, non-abutting dirty rects.
 *
 * Zero-initialisation is a valid empty set.
 */
typedef struct
{
    ERRect r[ER_DAMAGE_RECTS_MAX]; /**< The rects; only the first `count` entries are valid. */
    uint8_t count;                 /**< Number of valid rects; 0 = empty. */
} ERDamageSet;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (engine-internal)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Empties the set. */
void er_damage_set_clear(ERDamageSet* s);

/**
 * @brief Adds a rect to the set, preserving the disjointness invariant.
 *
 * An empty rect (w or h <= 0) is ignored. The input is merged (bounding-box union) with every stored
 * rect it overlaps or abuts, cascading until stable; when the set is already full, it is merged with
 * the stored rect that minimises wasted area instead of being dropped. Coverage is therefore always
 * conservative: every pixel of every added rect is covered by exactly one stored rect.
 *
 * @param[in] s  Set to add to.
 * @param[in] x  Rect left edge.
 * @param[in] y  Rect top edge.
 * @param[in] w  Rect width in pixels.
 * @param[in] h  Rect height in pixels.
 */
void er_damage_set_add(ERDamageSet* s, int x, int y, int w, int h);

/**
 * @brief Computes the bounding box of the whole set.
 *
 * @param[in]  s    Set to measure.
 * @param[out] out  Receives the union bbox; set to {0,0,0,0} when empty. May be NULL.
 *
 * @return true when the set is non-empty.
 */
bool er_damage_set_bounds(const ERDamageSet* s, ERRect* out);

/**
 * @brief Sums the areas of the stored rects (exact, since they are disjoint).
 *
 * @param[in] s  Set to measure.
 *
 * @return Total covered area in pixels; 0 when empty.
 */
uint32_t er_damage_set_area(const ERDamageSet* s);

#endif
