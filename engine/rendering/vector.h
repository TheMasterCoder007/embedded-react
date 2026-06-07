#ifndef EMBEDDED_REACT_VECTOR_H
#define EMBEDDED_REACT_VECTOR_H

#include "er_scene.h" /* ERVectorPaint + the ER_VOP / ER_VCAP / ER_VJOIN / ER_VFILL contract */

#include <stdint.h>

/*
 * Vector rasterizer + the engine-owned op-tape/paint storage pool for ER_NODE_VECTOR nodes.
 * The op-tape/paint encoding is the public contract in er_scene.h; this header is internal to the
 * engine (included by the compositor), exposing the rasterizer and the per-node storage slots.
 */

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
 * @param[in] px        Node box left edge in framebuffer pixels.
 * @param[in] py        Node box top edge in framebuffer pixels.
 * @param[in] w         Node box width in pixels.
 * @param[in] h         Node box height in pixels.
 */
void er_vector_render(
    const float* ops, int n_ops, const ERVectorPaint* paints, int n_paints, int px, int py, int w, int h);

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
int er_vector_store(int slot, const float* ops, int n_ops, const ERVectorPaint* paints, int n_paints);

/** @brief Releases a storage slot back to the pool (no-op for an invalid slot). */
void er_vector_free(int slot);

/** @brief Returns a slot's op-tape and writes its float count to @p n_ops (NULL/0 for an empty slot). */
const float* er_vector_slot_ops(int slot, int* n_ops);

/** @brief Returns a slot's paint table and writes its count to @p n_paints (NULL/0 for an empty slot). */
const ERVectorPaint* er_vector_slot_paints(int slot, int* n_paints);

#endif
