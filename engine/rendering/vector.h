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

#endif
