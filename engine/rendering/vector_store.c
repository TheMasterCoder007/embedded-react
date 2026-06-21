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

#ifndef ERUI_MAX_VECTOR_NODES
#define ERUI_MAX_VECTOR_NODES 8 /**< Concurrent vector nodes that can hold geometry. */
#endif
#ifndef ERUI_VECTOR_TAPE_MAX
#define ERUI_VECTOR_TAPE_MAX 1024 /**< Op-tape floats stored per vector node. */
#endif
#ifndef ERUI_VECTOR_PAINTS_MAX
#define ERUI_VECTOR_PAINTS_MAX 16 /**< Paint entries stored per vector node. */
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Pool
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    bool used;
    float ops[ERUI_VECTOR_TAPE_MAX];
    int n_ops;
    ERVectorPaint paints[ERUI_VECTOR_PAINTS_MAX];
    int n_paints;
} VecSlot;

static VecSlot s_slots[ERUI_MAX_VECTOR_NODES];

int er_vector_store(int slot, const float* ops, int n_ops, const ERVectorPaint* paints, int n_paints)
{
    if (slot < 0)
    {
        for (int i = 0; i < ERUI_MAX_VECTOR_NODES; i++)
        {
            if (!s_slots[i].used)
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
        {
            ERUI_VEC_WARN_ONCE("ERUI_MAX_VECTOR_NODES", ERUI_MAX_VECTOR_NODES);
            return -1; /* pool exhausted; node renders nothing */
        }
    }
    if (slot >= ERUI_MAX_VECTOR_NODES)
        return -1;

    VecSlot* s = &s_slots[slot];
    s->used = true;
    if (n_ops > ERUI_VECTOR_TAPE_MAX)
        ERUI_VEC_WARN_ONCE("ERUI_VECTOR_TAPE_MAX", ERUI_VECTOR_TAPE_MAX);
    int no = (n_ops < 0) ? 0 : (n_ops > ERUI_VECTOR_TAPE_MAX ? ERUI_VECTOR_TAPE_MAX : n_ops);
    if (ops && no > 0)
        memcpy(s->ops, ops, (size_t)no * sizeof(float));
    s->n_ops = no;
    if (n_paints > ERUI_VECTOR_PAINTS_MAX)
        ERUI_VEC_WARN_ONCE("ERUI_VECTOR_PAINTS_MAX", ERUI_VECTOR_PAINTS_MAX);
    int np = (n_paints < 0) ? 0 : (n_paints > ERUI_VECTOR_PAINTS_MAX ? ERUI_VECTOR_PAINTS_MAX : n_paints);
    if (paints && np > 0)
        memcpy(s->paints, paints, (size_t)np * sizeof(ERVectorPaint));
    s->n_paints = np;
    return slot;
}

void er_vector_free(int slot)
{
    if (slot >= 0 && slot < ERUI_MAX_VECTOR_NODES)
        s_slots[slot].used = false;
}

void er_vector_reset(void)
{
    /* Free every storage slot. The rasterizer's scratch (vector.c) is reset per shape inside
     * er_vector_render, so it needs no clearing here. */
    memset(s_slots, 0, sizeof(s_slots));
}

const float* er_vector_slot_ops(int slot, int* n_ops)
{
    if (slot < 0 || slot >= ERUI_MAX_VECTOR_NODES || !s_slots[slot].used)
    {
        if (n_ops)
            *n_ops = 0;
        return 0;
    }
    if (n_ops)
        *n_ops = s_slots[slot].n_ops;
    return s_slots[slot].ops;
}

const ERVectorPaint* er_vector_slot_paints(int slot, int* n_paints)
{
    if (slot < 0 || slot >= ERUI_MAX_VECTOR_NODES || !s_slots[slot].used)
    {
        if (n_paints)
            *n_paints = 0;
        return 0;
    }
    if (n_paints)
        *n_paints = s_slots[slot].n_paints;
    return s_slots[slot].paints;
}
