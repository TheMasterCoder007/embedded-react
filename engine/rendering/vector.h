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

#ifndef EMBEDDED_REACT_VECTOR_H
#define EMBEDDED_REACT_VECTOR_H

#include "er_scene.h" /* ERVectorPaint + the ER_VOP / ER_VCAP / ER_VJOIN / ER_VFILL contract */

#include <stdbool.h>
#include <stdint.h>

/*
 * Vector rasterizer + the engine-owned op-tape/paint storage pool for ER_NODE_VECTOR nodes.
 * The op-tape/paint encoding is the public contract in er_scene.h; this header is internal to the
 * engine (included by the compositor), exposing the rasterizer and the per-node storage slots.
 *
 * Implementation note: the rasterizer (vector.c) and the per-node storage pool (vector_store.c) are
 * SEPARATE translation units so a target can place the cold storage pool in slower far memory (e.g.
 * ESP32 PSRAM, via a linker fragment) while the hot per-pixel rasterize scratch stays in fast RAM.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Pool-overflow diagnostics (shared by the rasterizer + the storage pool)
 ---------------------------------------------------------------------------------------------------------------------*/

/* When a static pool is exhausted the vector code silently drops geometry — correct and memory-safe, but a
 * truncated shape is easy to mistake for a bug. With diagnostics on, the first overflow of each pool prints
 * a one-line warning naming the macro to raise. Defaults ON for debug builds and OFF when NDEBUG is defined,
 * so a release MCU pulls in no <stdio.h> and pays no code; force it with -DERUI_VECTOR_DIAGNOSTICS=0/1. */
#ifndef ERUI_VECTOR_DIAGNOSTICS
#ifdef NDEBUG
#define ERUI_VECTOR_DIAGNOSTICS 0
#else
#define ERUI_VECTOR_DIAGNOSTICS 1
#endif
#endif

#if ERUI_VECTOR_DIAGNOSTICS
#include <stdio.h>
/* Warn once per call site per process: an overflow can recur every frame, and one line is enough to act on.
 * The latch is static to each macro expansion, so each pool warns independently. */
#define ERUI_VEC_WARN_ONCE(macro_name, cap)                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        static bool er_vec_warned_ = false;                                                                            \
        if (!er_vec_warned_)                                                                                           \
        {                                                                                                              \
            er_vec_warned_ = true;                                                                                     \
            fprintf(stderr,                                                                                            \
                    "embedded-react vector: %s (%d) exhausted - shape truncated; raise it.\n",                         \
                    macro_name,                                                                                        \
                    (int)(cap));                                                                                       \
        }                                                                                                              \
    } while (0)
#else
#define ERUI_VEC_WARN_ONCE(macro_name, cap) ((void)0)
#endif

/* Running the STORAGE pool out of slots is the one overflow that stays hidden in a release build, so it
 * gets its own always-on knob. The caps above truncate a single shape — the screen shows something
 * recognisably wrong, and the shape that broke is the one you were editing. Exhausting the slot pool
 * instead denies a whole node its geometry: it draws nothing, and because slots are handed out in mount
 * order, WHICH nodes go missing shifts as screens mount and unmount. That reads on a panel as random
 * glitching with no obvious culprit and cost a full debugging cycle to trace, so it warns once per
 * process even under NDEBUG (and raises a flag the perf overlay shows). One fprintf on a path that has
 * already failed; force it off with -DERUI_VECTOR_STORE_WARN=0 on a target that must not link stdio. */
#ifndef ERUI_VECTOR_STORE_WARN
#define ERUI_VECTOR_STORE_WARN 1
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Edge cache (per-node cached rasterizer geometry; pool lives in vector_cache.c)
 ---------------------------------------------------------------------------------------------------------------------*/

/* Cache a static node's built rasterizer geometry (flattened + stroked edge lists) so repainting it —
 * e.g. every frame, under a moving sibling's damage rect — skips the tape parse, bezier/arc flattening
 * and stroke outlining and goes straight to the scanline rasterize. Set to 0 to compile the cache out
 * entirely (no pool, no record/replay code). The pool sizes (ERUI_VECTOR_CACHE_NODES / _EDGES /
 * _PASSES) and the entry layout are private to the engine — see vector_cache.h. */
#ifndef ERUI_VECTOR_EDGE_CACHE
#define ERUI_VECTOR_EDGE_CACHE 1
#endif

/* Opaque to the compositor and to tests: the layout depends on the private pool-size macros, so only
 * the two TUs that share it (vector.c / vector_cache.c, via vector_cache.h) may see it. */
typedef struct ERVecCache ERVecCache;

/**
 * @brief Returns the valid cache entry for (slot, px, py), or NULL on a miss.
 *
 * A hit refreshes the entry's LRU stamp and counts toward er_vector_cache_hits().
 */
const ERVecCache* er_vector_cache_lookup(int slot, int px, int py);

/**
 * @brief Claims an entry to record (slot, px, py) into, or returns NULL when this render should not
 *        record: the cache is compiled out, the slot's geometry overflowed the entry before (blocked
 *        until the slot is re-stored), or this is the FIRST render of the key — the first call arms
 *        the key and declines; only a later call with the key still intact is granted an entry.
 *
 * That last rule is the two-touch promotion: recording costs a full (unclipped) geometry build plus a
 * copy, which must never be added to an animated node's per-frame tape update — a key only proves it
 * is static by surviving from one render to the next (any er_vector_store() disarms it). The claimed
 * entry is invalid until er_vector_cache_finish(e, true).
 */
ERVecCache* er_vector_cache_begin(int slot, int px, int py);

/** @brief Ends a recording: ok=true publishes the entry, ok=false discards it and blocks the slot. */
void er_vector_cache_finish(ERVecCache* e, bool ok);

/** @brief Drops any cache entry (and pending promotion / block) for a storage slot. */
void er_vector_cache_invalidate_slot(int slot);

/** @brief Drops every cache entry (part of er_vector_reset()). */
void er_vector_cache_reset(void);

/** @brief Renders served from the cache since the last stats reset (0 when compiled out). */
uint32_t er_vector_cache_hits(void);

/** @brief Recordings published since the last stats reset (0 when compiled out). */
uint32_t er_vector_cache_builds(void);

/** @brief Zeroes the hit/build counters. */
void er_vector_cache_stats_reset(void);

/*----------------------------------------------------------------------------------------------------------------------
 - Rasterizer
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Rasterizes a vector op-tape into the active render target at a node's box.
 *
 * Each shape is filled (if its paint has a non-transparent fill) then stroked (if it has a
 * non-transparent stroke and positive width), in tape order. Anti-aliased coverage is composited via
 * the engine blit layer, so it blends correctly into the framebuffer and through opacity/transform
 * scratch. Painting is clipped to the node box [px, px+w) x [py, py+h).
 *
 * @param[in] ops       Flat op-tape (see the ER_VOP_* contract in er_scene.h).
 * @param[in] n_ops     Number of floats in @p ops.
 * @param[in] paints    Paint table (one ERVectorPaint per entry).
 * @param[in] n_paints  Number of paint entries.
 * @param[in] grads     Gradient table; a paint's 1-based fill_grad indexes it (NULL/0 when none). ERUI_GRADIENT.
 * @param[in] n_grads   Number of gradient entries.
 * @param[in] px        Geometry origin X in framebuffer pixels (node box left).
 * @param[in] py        Geometry origin Y in framebuffer pixels (node box top).
 * @param[in] clipx0    Clip box left edge (rasterize + paint are limited to this rect).
 * @param[in] clipy0    Clip box top edge.
 * @param[in] clipx1    Clip box right edge (exclusive).
 * @param[in] clipy1    Clip box bottom edge (exclusive).
 */
void er_vector_render(const float* ops,
                      int n_ops,
                      const ERVectorPaint* paints,
                      int n_paints,
                      const ERVectorGradient* grads,
                      int n_grads,
                      int px,
                      int py,
                      int clipx0,
                      int clipy0,
                      int clipx1,
                      int clipy1);

/**
 * @brief Renders a storage slot's geometry at a node's box, through the edge cache when possible.
 *
 * The compositor's entry point for ER_NODE_VECTOR. Same painting contract as er_vector_render() with
 * the slot's stored tape/paints/gradients — but with the slot identity in hand it can cache the built
 * edge lists: a repaint of an unchanged node (same tape, same origin) replays the cached geometry
 * instead of re-flattening and re-stroking the tape. See ERUI_VECTOR_EDGE_CACHE; with the cache
 * compiled out (or on a miss) this renders exactly like er_vector_render().
 *
 * @param[in] slot      Storage slot holding the node's tape (er_vector_store); no-op when empty.
 * @param[in] px        Geometry origin X in framebuffer pixels (node box left).
 * @param[in] py        Geometry origin Y in framebuffer pixels (node box top).
 * @param[in] clipx0    Clip box left edge — the caller passes node box ∩ damage clip.
 * @param[in] clipy0    Clip box top edge.
 * @param[in] clipx1    Clip box right edge (exclusive).
 * @param[in] clipy1    Clip box bottom edge (exclusive).
 */
void er_vector_render_slot(int slot, int px, int py, int clipx0, int clipy0, int clipx1, int clipy1);

/**
 * @brief Number of shapes er_vector_render() has routed to the shared analytic arc core since the last
 *        reset (ERUI_VECTOR_ANALYTIC_ARC; always 0 when that is compiled out).
 *
 * A diagnostic for WHICH route a shape took, which is otherwise invisible: an `<Arc>` / `<Circle>` whose
 * paint the sector core cannot express (a gradient, a square cap, a filled partial arc) silently falls
 * back to the general tessellated path. Tests assert the routing decision with it.
 *
 * @return Monotonic count since er_vector_analytic_arc_count_reset().
 */
uint32_t er_vector_analytic_arc_count(void);

/**
 * @brief Zeroes the analytic-arc routing counter.
 */
void er_vector_analytic_arc_count_reset(void);

/*----------------------------------------------------------------------------------------------------------------------
 - Per-node storage pool (a fixed set of slots; a vector node references one by index)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Copies an op-tape + paint table into a storage slot, allocating one if needed.
 *
 * @param[in] slot      Existing slot index to overwrite, or < 0 to allocate a free one.
 * @param[in] ops       Op-tape to copy (clamped to the slot capacity).
 * @param[in] n_ops     Float count in @p ops.
 * @param[in] paints    Paint table to copy (clamped to capacity).
 * @param[in] n_paints  Paint count.
 *
 * @return The slot index now holding the data, or -1 if no slot was available.
 */
int er_vector_store(int slot,
                    const float* ops,
                    int n_ops,
                    const ERVectorPaint* paints,
                    int n_paints,
                    const ERVectorGradient* grads,
                    int n_grads);

/** @brief Releases a storage slot back to the pool (no-op for an invalid slot). */
void er_vector_free(int slot);

/** @brief Releases every storage slot back to the pool (part of er_reset()). */
void er_vector_reset(void);

/** @brief Returns a slot's op-tape and writes its float count to @p n_ops (NULL/0 for an empty slot). */
const float* er_vector_slot_ops(int slot, int* n_ops);

/** @brief Returns a slot's paint table and writes its count to @p n_paints (NULL/0 for an empty slot). */
const ERVectorPaint* er_vector_slot_paints(int slot, int* n_paints);

/** @brief Returns a slot's gradient table and writes its count to @p n_grads (NULL/0 when none / no ERUI_GRADIENT). */
const ERVectorGradient* er_vector_slot_grads(int slot, int* n_grads);

/** @brief Storage slots currently holding geometry — the perf overlay's "vector slots in use" counter. */
int er_vector_slots_in_use(void);

/** @brief Total storage slots compiled in (ERUI_MAX_VECTOR_NODES); the denominator for the above. */
int er_vector_slots_total(void);

/**
 * @brief True once a node has been denied a storage slot, i.e. the pool ran out.
 *
 * Sticky rather than momentary: the store fails at commit time, but the consequence — a vector node
 * left with no geometry — persists for as long as that node is mounted, so a per-frame sample would
 * miss the event that explains what is on screen. Cleared only by er_vector_reset() (er_reset()).
 * Surfaced per frame as ERPerfFrame::vector_slots_overflow / the overlay's "VEC n/n!FULL".
 */
bool er_vector_slots_overflowed(void);

#endif
