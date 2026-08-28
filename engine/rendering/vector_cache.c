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
 * Per-node EDGE CACHE for ER_NODE_VECTOR nodes — the pool behind ERUI_VECTOR_EDGE_CACHE. A static
 * <Svg> that keeps getting repainted (a moving sibling's damage rect crossing it, or the same damage
 * replayed into each buffer of a multi-buffer display) used to re-parse, re-flatten and re-stroke its
 * whole tape every time; this pool keeps the finished edge lists so those repaints go straight to the
 * scanline rasterize. vector.c records into and replays from these entries (er_vector_render_slot);
 * this file owns only the storage, the (slot, origin) keying and the LRU.
 *
 * A separate translation unit — like vector_store.c — so a target can place the pool independently of
 * vector.c's hot scratch. NOTE the trade-off is different from the store's: cached edges are read in
 * the rasterizer's per-scanline crossing loop during a replay, so placing this pool in far memory
 * (e.g. ESP32 PSRAM) trades some replay speed for internal RAM. That is usually still a large win over
 * re-tessellating, but a target with the internal RAM to spare should keep this pool there.
 *
 * Footprint: ERUI_VECTOR_CACHE_NODES x (ERUI_VECTOR_CACHE_EDGES x sizeof(ERVecEdge)
 *            + ERUI_VECTOR_CACHE_PASSES x sizeof(ERVecPass)); zero when ERUI_VECTOR_EDGE_CACHE=0.
 *
 * Invalidation is eager and lives with the store's mutators: er_vector_store()/er_vector_free() call
 * er_vector_cache_invalidate_slot(), er_vector_reset() calls er_vector_cache_reset(). An entry is
 * additionally keyed on the node origin (px, py) — edges bake the screen position in — so a node that
 * moved simply misses and rebuilds.
 */

#include "vector_cache.h" /* the entry layout + pool-size tunables (shared with vector.c only) */

#include <stdbool.h>

#if ERUI_VECTOR_EDGE_CACHE

static ERVecCache s_entries[ERUI_VECTOR_CACHE_NODES];

/**
 * @brief Per-storage-slot promotion state for the two-touch rule (see er_vector_cache_begin in
 *        vector.h): a key must MISS once and come back unchanged before a recording is allowed, so an
 *        animated node — whose tape update invalidates this state every frame — never pays the
 *        record-mode geometry build. `blocked` latches a recording that overflowed the entry: the
 *        node's geometry does not fit, so stop re-trying every frame until the slot is re-stored.
 */
typedef struct
{
    bool armed;   /**< A previous render already missed on (px, py) and no store has intervened since. */
    bool blocked; /**< Geometry overflowed the entry; do not record again for this tape. */
    int px, py;
} VecCachePending;

static VecCachePending s_pending[ERUI_MAX_VECTOR_NODES > 0 ? ERUI_MAX_VECTOR_NODES : 1];

static uint32_t s_tick;             /* LRU clock: bumped on every hit/claim */
static uint32_t s_hits, s_builds;   /* stats for tests + tuning */
static ERVecCache* s_recording = 0; /* entry claimed by an in-flight recording (at most one) */

/** @brief Marks every entry empty on first use — .bss zeroing leaves slot 0, which is a real slot. */
static void ensure_init(void)
{
    static bool done = false;
    if (done)
        return;
    done = true;
    for (int i = 0; i < ERUI_VECTOR_CACHE_NODES; i++)
        s_entries[i].slot = -1;
}

/** @brief The entry currently caching @p slot, valid or not (NULL when none). */
static ERVecCache* entry_for_slot(int slot)
{
    for (int i = 0; i < ERUI_VECTOR_CACHE_NODES; i++)
        if (s_entries[i].slot == slot)
            return &s_entries[i];
    return 0;
}

const ERVecCache* er_vector_cache_lookup(int slot, int px, int py)
{
    if (slot < 0)
        return 0;
    ensure_init();
    ERVecCache* e = entry_for_slot(slot);
    if (e && e->valid && e->px == px && e->py == py)
    {
        e->stamp = ++s_tick;
        s_hits++;
        return e;
    }
    return 0;
}

ERVecCache* er_vector_cache_begin(int slot, int px, int py)
{
    if (slot < 0 || slot >= ERUI_MAX_VECTOR_NODES || s_recording)
        return 0;
    ensure_init();
    VecCachePending* p = &s_pending[slot];
    if (p->blocked)
        return 0;
    if (!p->armed || p->px != px || p->py != py)
    {
        /* First touch of this key: arm it and decline. Recording is only granted on the NEXT render —
         * if the tape changes in between, the store's invalidate disarms, so an animated node (whose
         * tape updates every frame) never reaches a recording. This is the two-touch promotion. */
        p->armed = true;
        p->px = px;
        p->py = py;
        return 0;
    }

    /* Prefer the entry already tied to this slot (it is stale), then an empty one, then evict LRU. */
    ERVecCache* e = entry_for_slot(slot);
    if (!e)
        e = entry_for_slot(-1);
    if (!e)
    {
        e = &s_entries[0];
        for (int i = 1; i < ERUI_VECTOR_CACHE_NODES; i++)
            if (s_entries[i].stamp < e->stamp)
                e = &s_entries[i];
    }
    e->slot = slot;
    e->valid = false;
    e->px = px;
    e->py = py;
    e->stamp = ++s_tick;
    e->n_passes = 0;
    e->n_edges = 0;
    s_recording = e;
    return e;
}

void er_vector_cache_finish(ERVecCache* e, bool ok)
{
    if (!e)
        return;
    if (e == s_recording)
        s_recording = 0;
    if (ok)
    {
        e->valid = true;
        s_builds++;
        return;
    }
    /* The geometry did not fit (or the build truncated): free the entry and stop re-trying this tape —
     * a recording that always fails would otherwise add its full-geometry build to every frame. A
     * re-store of the slot clears the block (the new tape may fit). */
    if (e->slot >= 0 && e->slot < ERUI_MAX_VECTOR_NODES)
    {
        s_pending[e->slot].armed = false;
        s_pending[e->slot].blocked = true;
    }
    e->slot = -1;
    e->valid = false;
    e->stamp = 0; /* evict first */
}

void er_vector_cache_invalidate_slot(int slot)
{
    if (slot < 0)
        return;
    ensure_init();
    ERVecCache* e = entry_for_slot(slot);
    if (e)
    {
        e->slot = -1;
        e->valid = false;
        e->stamp = 0;
        if (e == s_recording)
            s_recording = 0;
    }
    if (slot < ERUI_MAX_VECTOR_NODES)
    {
        s_pending[slot].armed = false;
        s_pending[slot].blocked = false;
    }
}

void er_vector_cache_reset(void)
{
    ensure_init();
    for (int i = 0; i < ERUI_VECTOR_CACHE_NODES; i++)
    {
        s_entries[i].slot = -1;
        s_entries[i].valid = false;
        s_entries[i].stamp = 0;
    }
    for (int i = 0; i < ERUI_MAX_VECTOR_NODES; i++)
    {
        s_pending[i].armed = false;
        s_pending[i].blocked = false;
    }
    s_recording = 0;
    /* Stats are per process (like the warn-once latches), not per scene: not cleared here. */
}

uint32_t er_vector_cache_hits(void)
{
    return s_hits;
}

uint32_t er_vector_cache_builds(void)
{
    return s_builds;
}

void er_vector_cache_stats_reset(void)
{
    s_hits = 0;
    s_builds = 0;
}

#else /* !ERUI_VECTOR_EDGE_CACHE — no pool, no state; every call is a cheap no-op */

const ERVecCache* er_vector_cache_lookup(int slot, int px, int py)
{
    (void)slot;
    (void)px;
    (void)py;
    return 0;
}

ERVecCache* er_vector_cache_begin(int slot, int px, int py)
{
    (void)slot;
    (void)px;
    (void)py;
    return 0;
}

void er_vector_cache_finish(ERVecCache* e, bool ok)
{
    (void)e;
    (void)ok;
}

void er_vector_cache_invalidate_slot(int slot)
{
    (void)slot;
}

void er_vector_cache_reset(void)
{
}

uint32_t er_vector_cache_hits(void)
{
    return 0;
}

uint32_t er_vector_cache_builds(void)
{
    return 0;
}

void er_vector_cache_stats_reset(void)
{
}

#endif /* ERUI_VECTOR_EDGE_CACHE */
