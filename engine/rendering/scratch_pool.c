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

#include "scratch_pool.h"
#include "renderer_internal.h"
#include <string.h>

#ifndef ERUI_SCRATCH_W
#define ERUI_SCRATCH_W 240
#endif
#ifndef ERUI_SCRATCH_H
#define ERUI_SCRATCH_H 240
#endif
#ifndef ERUI_SCRATCH_BAND_H
#define ERUI_SCRATCH_BAND_H ERUI_SCRATCH_H
#endif
#ifndef ERUI_MAX_OPACITY_DEPTH
#define ERUI_MAX_OPACITY_DEPTH 4
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Metadata for one active scratch slot: its world-space origin and the draw alpha
 *        that was inherited when the capture started.
 */
typedef struct
{
    int ox;              /**< World-space X of scratch[0]. */
    int oy;              /**< World-space Y of scratch[0]. */
    uint8_t saved_alpha; /**< Inherited draw alpha to re-apply when the slot is blended out. */
} ScratchMeta;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/* Base scratch: bottom-of-stack redirects installed by offscreen captures (the transform
 * subsystem's source capture, the compositor's fade cache). When the opacity depth drops to
 * zero, blit calls route to the top base instead of the framebuffer. Two levels: a transform
 * source capture can occur inside a fade-cache capture. */
typedef struct
{
    uint32_t* buf;
    int w;
    int h;
    int ox;
    int oy;
} BaseEntry;

/* Each slot is one strip of ERUI_SCRATCH_W × ERUI_SCRATCH_BAND_H premultiplied ARGB8888 pixels.
 * ONE physical pool for all render workers — internal RAM is too tight to duplicate it — carved
 * into ERUI_RENDER_WORKERS equal windows of ER_SLOTS_PER_WORKER slots each, so concurrent
 * composites never share a slot. With the default single worker the window IS the whole pool. */
static uint32_t s_pool[ERUI_MAX_OPACITY_DEPTH][ERUI_SCRATCH_BAND_H * ERUI_SCRATCH_W];

#define ER_SLOTS_PER_WORKER (ERUI_MAX_OPACITY_DEPTH / ERUI_RENDER_WORKERS)
#if ERUI_RENDER_WORKERS > ERUI_MAX_OPACITY_DEPTH
#error "ERUI_RENDER_WORKERS must not exceed ERUI_MAX_OPACITY_DEPTH (each worker needs >= 1 opacity slot)"
#endif

/**
 * @brief Per-worker scratch-pool bookkeeping (see er_render_worker_id).
 *
 * The stacks are balanced within one worker's render traversal, so each worker gets its own
 * copy; only the slot pixels live in the shared (windowed) pool above.
 */
typedef struct
{
    ScratchMeta meta[ER_SLOTS_PER_WORKER]; /**< Origin + saved alpha per active slot. */
    int depth;                             /**< Active opacity captures, within this worker's window. */
    BaseEntry base[2];                     /**< Base-redirect stack (transform source / fade cache). */
    int base_count;
} PoolCtx;

static PoolCtx s_ctx[ERUI_RENDER_WORKERS];

/** @brief The calling worker's pool context (constant &s_ctx[0] in single-worker builds). */
static inline PoolCtx* pc(void)
{
    return &s_ctx[er_render_worker_id()];
}

/** @brief Pixel storage for the calling worker's slot at the given in-window depth. */
static inline uint32_t* slot_px(int depth)
{
    return s_pool[er_render_worker_id() * ER_SLOTS_PER_WORKER + depth];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_scratch_push_base(uint32_t* buf, int w, int h, int ox, int oy)
{
    if (pc()->base_count >= (int)(sizeof(pc()->base) / sizeof(pc()->base[0])))
        return; /* callers guard nesting; defensive */
    pc()->base[pc()->base_count].buf = buf;
    pc()->base[pc()->base_count].w = w;
    pc()->base[pc()->base_count].h = h;
    pc()->base[pc()->base_count].ox = ox;
    pc()->base[pc()->base_count].oy = oy;
    pc()->base_count++;
    er_scratch_begin(buf, w, h, ox, oy);
}

void er_scratch_pop_base(void)
{
    if (pc()->base_count > 0)
        pc()->base_count--;
    /* Restore routing: the active opacity slot, then the next outer base, then clear. */
    if (pc()->depth > 0)
        er_scratch_begin(
            slot_px(pc()->depth - 1), ERUI_SCRATCH_W, ERUI_SCRATCH_BAND_H, pc()->meta[pc()->depth - 1].ox, pc()->meta[pc()->depth - 1].oy);
    else if (pc()->base_count > 0)
        er_scratch_begin(pc()->base[pc()->base_count - 1].buf,
                         pc()->base[pc()->base_count - 1].w,
                         pc()->base[pc()->base_count - 1].h,
                         pc()->base[pc()->base_count - 1].ox,
                         pc()->base[pc()->base_count - 1].oy);
    else
        er_scratch_end();
}

bool er_scratch_idle(void)
{
    return pc()->depth == 0 && pc()->base_count == 0;
}

int er_scratch_strip_w(void)
{
    return ERUI_SCRATCH_W;
}

int er_scratch_strip_h(void)
{
    return ERUI_SCRATCH_BAND_H;
}

bool er_scratch_avail(void)
{
    return pc()->depth < ER_SLOTS_PER_WORKER;
}

bool er_scratch_push(int x, int y, int w, int h)
{
    if (pc()->depth >= ER_SLOTS_PER_WORKER)
        return false;
    if (w <= 0 || h <= 0 || w > ERUI_SCRATCH_W || h > ERUI_SCRATCH_BAND_H)
        return false;

    uint32_t* slot = slot_px(pc()->depth);
    /* Clear only the node's w×h footprint, not the whole slot. The slot is begun with origin (x,y),
     * so the node renders into scratch-local [0,0,w,h], and er_scratch_pop_blend reads back exactly
     * that region (er_blit_blend only advances into the slot by the clip delta, never past
     * [0,0,w,h]); anything a child overflows beyond it is never blended back. */
    for (int row = 0; row < h; row++)
        memset(slot + (size_t)row * (size_t)ERUI_SCRATCH_W, 0, (size_t)w * sizeof(uint32_t));
    pc()->meta[pc()->depth].ox = x;
    pc()->meta[pc()->depth].oy = y;
    pc()->meta[pc()->depth].saved_alpha = er_get_draw_alpha();
    er_set_draw_alpha(255U);
    pc()->depth++;

    er_scratch_begin(slot, ERUI_SCRATCH_W, ERUI_SCRATCH_BAND_H, x, y);
    return true;
}

void er_scratch_pop_blend(uint8_t alpha, int x, int y, int w, int h)
{
    if (pc()->depth <= 0)
        return;

    pc()->depth--;
    er_scratch_end();

    /* Restore routing: prefer the next outer opacity slot, then the transform base, then NULL. */
    if (pc()->depth > 0)
    {
        er_scratch_begin(
            slot_px(pc()->depth - 1), ERUI_SCRATCH_W, ERUI_SCRATCH_BAND_H, pc()->meta[pc()->depth - 1].ox, pc()->meta[pc()->depth - 1].oy);
    }
    else if (pc()->base_count > 0)
    {
        er_scratch_begin(pc()->base[pc()->base_count - 1].buf,
                         pc()->base[pc()->base_count - 1].w,
                         pc()->base[pc()->base_count - 1].h,
                         pc()->base[pc()->base_count - 1].ox,
                         pc()->base[pc()->base_count - 1].oy);
    }

    /* Re-apply the inherited draw alpha before blending the captured strip out, so a group
     * composited inside a degraded (multiplied-alpha) ancestor is dimmed exactly once. */
    er_set_draw_alpha(pc()->meta[pc()->depth].saved_alpha);

    const uint32_t* slot = slot_px(pc()->depth);
    const int stride = (int)(ERUI_SCRATCH_W * sizeof(uint32_t));
    er_blit_blend(slot, stride, alpha, x, y, w, h);
}
