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

#ifndef EMBEDDED_REACT_VECTOR_CACHE_H
#define EMBEDDED_REACT_VECTOR_CACHE_H

#include "vector.h"

/*
 * Layout of the vector edge cache — shared ONLY by vector.c (which records and replays entries) and
 * vector_cache.c (which owns the pool). Everyone else sees ERVecCache as an opaque type through
 * vector.h: the sizes below are private compile-time knobs, so a TU built with different values (a
 * test, a consumer) must never see this layout.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Tunables
 ---------------------------------------------------------------------------------------------------------------------*/

#ifndef ERUI_VECTOR_CACHE_NODES
#define ERUI_VECTOR_CACHE_NODES 2 /**< Nodes whose geometry is cached at once (LRU across vector nodes). */
#endif
#ifndef ERUI_VECTOR_CACHE_EDGES
#define ERUI_VECTOR_CACHE_EDGES                                                                                        \
    4096 /**< Edges cached per node — the SUM over all its fill+stroke passes, so                                    \
            larger than the per-pass ERUI_VECTOR_MAX_EDGES: a decorated face (60                                       \
            round-capped ticks + a few filled+stroked paths) measures ~2.5-3k. */
#endif
#ifndef ERUI_VECTOR_CACHE_PASSES
#define ERUI_VECTOR_CACHE_PASSES 48 /**< Rasterize passes cached per node (fill + stroke per shape, + arcs). */
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Entry layout
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One rasterizer edge, normalized so y0 <= y1. Built by vector.c's geometry stage; a cache
 *        entry stores the finished lists and replay hands them straight back to the rasterizer.
 */
typedef struct
{
    float x0, y0, x1, y1;
    int dir; /**< +1 if the edge originally went downward (y increasing), else -1. */
} ERVecEdge;

/** @brief What one cached pass replays as. */
enum
{
    ER_VEC_PASS_FILL = 0,   /**< rasterize cached edges with the paint's fill color/gradient + fill rule */
    ER_VEC_PASS_STROKE = 1, /**< rasterize cached edges with the paint's stroke color/gradient (nonzero) */
    ER_VEC_PASS_ARC = 2     /**< re-route to the analytic arc core (no edges; params below) */
};

/**
 * @brief One recorded rasterize pass. FILL/STROKE reference a range of the entry's edge pool; ARC
 *        carries the analytic sector parameters directly (the arc core is already clip-cheap, so its
 *        geometry is re-derived from these rather than tessellated).
 */
typedef struct
{
    uint8_t kind;             /**< ER_VEC_PASS_*. */
    uint16_t paint;           /**< Index into the node's paint table (colors/gradients resolve at replay). */
    int32_t start;            /**< First edge in the entry's pool (FILL/STROKE). */
    int32_t count;            /**< Edge count (FILL/STROKE). */
    float bx0, by0, bx1, by1; /**< Ink bounds — replay skips the pass when a clip cannot see it. */
    float arc_cx, arc_cy, arc_r, arc_a0_deg, arc_sweep_deg; /**< ARC only. */
    bool arc_full;                                          /**< ARC only: sweep covers the whole circle. */
} ERVecPass;

/**
 * @brief One cached node: the pass list plus the edge pool the passes index into.
 *
 * Valid while (slot, px, py) still describe the node: any er_vector_store()/er_vector_free() on the
 * slot invalidates it eagerly, and a node that moved rebuilds (edges bake the screen origin in).
 */
struct ERVecCache
{
    int slot;   /**< Storage slot this entry mirrors; -1 = empty. */
    bool valid; /**< False while recording (and after a failed recording). */
    int px, py; /**< Node origin the edges were built at. */
    uint32_t stamp;
    int n_passes;
    int n_edges;
    ERVecPass passes[ERUI_VECTOR_CACHE_PASSES];
    ERVecEdge edges[ERUI_VECTOR_CACHE_EDGES];
};

#endif
