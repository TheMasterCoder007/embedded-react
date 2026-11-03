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
 * Per-node op-tape/paint STORAGE pool for ER_NODE_VECTOR nodes — split out of vector.c (the rasterizer)
 * into its own translation unit on purpose: this pool's static array (s_slots) is COLD relative to the
 * rasterizer's per-pixel scratch (it's read once per node when re-rasterizing, not in the scanline inner
 * loop). Keeping it a separate object lets a target place it in slower far memory — e.g. ESP32 PSRAM, via
 * a linker fragment mapping vector_store.o's .bss to external RAM — while vector.o's hot scratch stays in
 * fast internal RAM. On a target without that mapping it simply links alongside the rest of the engine.
 *
 * Footprint: ERUI_MAX_VECTOR_NODES × (ERUI_VECTOR_TAPE_MAX × 4 + ERUI_VECTOR_PAINTS_MAX × sizeof(paint)).
 * With the pool in PSRAM, ERUI_MAX_VECTOR_NODES can be raised well past the internal-RAM-bound default.
 */

#include "vector.h" /* er_vector_store/... decls, ERVectorPaint, and the ERUI_VEC_WARN_ONCE diagnostics macro */

#include <stdbool.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Tunables (the storage pool's caps; the rasterizer's caps live in vector.c)
 ---------------------------------------------------------------------------------------------------------------------*/

/* ERUI_MAX_VECTOR_NODES is in vector.h, since the edge cache sizes by it too.
 * ERUI_VECTOR_TAPE_MAX / _PAINTS_MAX / _GRADS_MAX are declared in er_scene.h (via vector.h): callers
 * marshal into them, so they are published rather than private to this pool. */

/*----------------------------------------------------------------------------------------------------------------------
 - Pool
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    bool used;
    float ops[ERUI_VECTOR_TAPE_MAX];
    int op_count;
    ERVectorPaint paints[ERUI_VECTOR_PAINTS_MAX];
    int paint_count;
#if ERUI_GRADIENT
    ERVectorGradient gradients[ERUI_VECTOR_GRADS_MAX]; /**< Gradient table; a paint's fill_grad indexes it (1-based). */
    int gradient_count;
#endif
} VecSlot;

static VecSlot s_slots[ERUI_MAX_VECTOR_NODES];

/* Sticky: set the first time a node is denied a slot, cleared only by er_vector_reset(). @see
 * er_vector_slots_overflowed. */
static bool s_pool_overflowed = false;

/**
 * @brief Records pool exhaustion: raises the sticky flag and warns once per process.
 *
 * Unlike the per-shape caps, this one warns in RELEASE builds too — the symptom (a node with no
 * geometry, so nothing drawn) carries no hint of its cause. @see ERUI_DIAGNOSTICS.
 */
static void note_pool_overflow(void)
{
    s_pool_overflowed = true;
    ERUI_WARN_ONCE_RELEASE("embedded-react vector: ERUI_MAX_VECTOR_NODES (%d) exhausted - this <Svg> node and any "
                           "further one will draw NOTHING until a slot frees; raise it.\n",
                           (int)ERUI_MAX_VECTOR_NODES);
}

int er_vector_store(int slot,
                    const float* ops,
                    int op_count,
                    const ERVectorPaint* paints,
                    int paint_count,
                    const ERVectorGradient* gradients,
                    int gradient_count)
{
    if (slot < 0)
    {
        for (int slot_index = 0; slot_index < ERUI_MAX_VECTOR_NODES; slot_index++)
        {
            if (!s_slots[slot_index].used)
            {
                slot = slot_index;
                break;
            }
        }
        if (slot < 0)
        {
            note_pool_overflow();
            return -1; /* pool exhausted; node renders nothing */
        }
    }
    if (slot >= ERUI_MAX_VECTOR_NODES)
        return -1;

    /* The tape/paints are changing: any cached edge lists built from the old contents are stale.
     * (Also clears the cache's per-slot promotion state, so an animated tape never records.) */
    er_vector_cache_invalidate_slot(slot);

    VecSlot* stored = &s_slots[slot];
    stored->used = true;
    if (op_count > ERUI_VECTOR_TAPE_MAX)
        ERUI_VEC_WARN_ONCE("ERUI_VECTOR_TAPE_MAX", ERUI_VECTOR_TAPE_MAX);
    int stored_op_count = (op_count < 0) ? 0 : (op_count > ERUI_VECTOR_TAPE_MAX ? ERUI_VECTOR_TAPE_MAX : op_count);
    if (ops && stored_op_count > 0)
        memcpy(stored->ops, ops, (size_t)stored_op_count * sizeof(float));
    stored->op_count = stored_op_count;
    if (paint_count > ERUI_VECTOR_PAINTS_MAX)
        ERUI_VEC_WARN_ONCE("ERUI_VECTOR_PAINTS_MAX", ERUI_VECTOR_PAINTS_MAX);
    int stored_paint_count =
        (paint_count < 0) ? 0 : (paint_count > ERUI_VECTOR_PAINTS_MAX ? ERUI_VECTOR_PAINTS_MAX : paint_count);
    if (paints && stored_paint_count > 0)
        memcpy(stored->paints, paints, (size_t)stored_paint_count * sizeof(ERVectorPaint));
    stored->paint_count = stored_paint_count;
#if ERUI_GRADIENT
    if (gradient_count > ERUI_VECTOR_GRADS_MAX)
        ERUI_VEC_WARN_ONCE("ERUI_VECTOR_GRADS_MAX", ERUI_VECTOR_GRADS_MAX);
    int stored_gradient_count =
        (gradient_count < 0) ? 0 : (gradient_count > ERUI_VECTOR_GRADS_MAX ? ERUI_VECTOR_GRADS_MAX : gradient_count);
    if (gradients && stored_gradient_count > 0)
        memcpy(stored->gradients, gradients, (size_t)stored_gradient_count * sizeof(ERVectorGradient));
    stored->gradient_count = stored_gradient_count;
#else
    (void)gradients;
    (void)gradient_count;
#endif
    return slot;
}

void er_vector_free(int slot)
{
    if (slot >= 0 && slot < ERUI_MAX_VECTOR_NODES)
    {
        s_slots[slot].used = false;
        er_vector_cache_invalidate_slot(slot); /* the slot may be re-issued to a different node */
    }
}

int er_vector_capacity(void)
{
    return ERUI_MAX_VECTOR_NODES;
}

void er_vector_reset(void)
{
    /* Free every storage slot. The rasterizer's scratch (vector.c) is reset per shape inside
     * er_vector_render, so it needs no clearing here. */
    memset(s_slots, 0, sizeof(s_slots));
    er_vector_cache_reset(); /* every cached edge list described geometry that is now gone */
    /* The overflow flag describes the geometry that was in the pool, so it goes with it — a fresh scene
     * gets a fresh verdict. The one-shot warning latch is deliberately NOT cleared: it is per process. */
    s_pool_overflowed = false;
}

const float* er_vector_slot_ops(int slot, int* op_count)
{
    if (slot < 0 || slot >= ERUI_MAX_VECTOR_NODES || !s_slots[slot].used)
    {
        if (op_count)
            *op_count = 0;
        return 0;
    }
    if (op_count)
        *op_count = s_slots[slot].op_count;
    return s_slots[slot].ops;
}

const ERVectorPaint* er_vector_slot_paints(int slot, int* paint_count)
{
    if (slot < 0 || slot >= ERUI_MAX_VECTOR_NODES || !s_slots[slot].used)
    {
        if (paint_count)
            *paint_count = 0;
        return 0;
    }
    if (paint_count)
        *paint_count = s_slots[slot].paint_count;
    return s_slots[slot].paints;
}

const ERVectorGradient* er_vector_slot_grads(int slot, int* gradient_count)
{
#if ERUI_GRADIENT
    if (slot < 0 || slot >= ERUI_MAX_VECTOR_NODES || !s_slots[slot].used)
    {
        if (gradient_count)
            *gradient_count = 0;
        return 0;
    }
    if (gradient_count)
        *gradient_count = s_slots[slot].gradient_count;
    return s_slots[slot].gradients;
#else
    (void)slot;
    if (gradient_count)
        *gradient_count = 0;
    return 0;
#endif
}

int er_vector_slots_in_use(void)
{
    int in_use_count = 0;
    for (int slot_index = 0; slot_index < ERUI_MAX_VECTOR_NODES; slot_index++)
        if (s_slots[slot_index].used)
            in_use_count++;
    return in_use_count;
}

int er_vector_slots_total(void)
{
    return ERUI_MAX_VECTOR_NODES;
}

bool er_vector_slots_overflowed(void)
{
    return s_pool_overflowed;
}
