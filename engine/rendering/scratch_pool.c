#include "scratch_pool.h"
#include "renderer_internal.h"
#include <string.h>

#ifndef ERUI_SCRATCH_W
#define ERUI_SCRATCH_W 240
#endif
#ifndef ERUI_SCRATCH_H
#define ERUI_SCRATCH_H 240
#endif
#ifndef ERUI_MAX_OPACITY_DEPTH
#define ERUI_MAX_OPACITY_DEPTH 4
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Metadata for one active scratch slot: its world-space origin.
 */
typedef struct
{
    int ox; /**< World-space X of scratch[0]. */
    int oy; /**< World-space Y of scratch[0]. */
} ScratchMeta;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/* Each slot is ERUI_SCRATCH_W × ERUI_SCRATCH_H premultiplied ARGB8888 pixels. */
static uint32_t s_pool[ERUI_MAX_OPACITY_DEPTH][ERUI_SCRATCH_H * ERUI_SCRATCH_W];
static ScratchMeta s_meta[ERUI_MAX_OPACITY_DEPTH];
static int s_depth = 0;

/* Base scratch: a bottom-of-stack redirect installed by the transform subsystem.
 * When the opacity depth drops to zero, blit calls route here instead of to the framebuffer. */
static uint32_t* s_base_buf = NULL;
static int s_base_w = 0;
static int s_base_h = 0;
static int s_base_ox = 0;
static int s_base_oy = 0;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_scratch_push_base(uint32_t* buf, int w, int h, int ox, int oy)
{
    s_base_buf = buf;
    s_base_w = w;
    s_base_h = h;
    s_base_ox = ox;
    s_base_oy = oy;
    er_scratch_begin(buf, w, h, ox, oy);
}

void er_scratch_pop_base(void)
{
    s_base_buf = NULL;
    /* Restore routing to the active opacity slot, or clear if none. */
    if (s_depth > 0)
        er_scratch_begin(
            s_pool[s_depth - 1], ERUI_SCRATCH_W, ERUI_SCRATCH_H, s_meta[s_depth - 1].ox, s_meta[s_depth - 1].oy);
    else
        er_scratch_end();
}

bool er_scratch_push(int x, int y, int w, int h)
{
    if (s_depth >= ERUI_MAX_OPACITY_DEPTH)
        return false;
    if (w <= 0 || h <= 0 || w > ERUI_SCRATCH_W || h > ERUI_SCRATCH_H)
        return false;

    uint32_t* slot = s_pool[s_depth];
    /* Clear only the node's w×h footprint, not the whole slot. The slot is begun with origin (x,y),
     * so the node renders into scratch-local [0,0,w,h], and er_scratch_pop_blend reads back exactly
     * that region (er_blit_blend only advances into the slot by the clip delta, never past
     * [0,0,w,h]); anything a child overflows beyond it is never blended back. */
    for (int row = 0; row < h; row++)
        memset(slot + (size_t)row * (size_t)ERUI_SCRATCH_W, 0, (size_t)w * sizeof(uint32_t));
    s_meta[s_depth].ox = x;
    s_meta[s_depth].oy = y;
    s_depth++;

    er_scratch_begin(slot, ERUI_SCRATCH_W, ERUI_SCRATCH_H, x, y);
    return true;
}

void er_scratch_pop_blend(uint8_t alpha, int x, int y, int w, int h)
{
    if (s_depth <= 0)
        return;

    s_depth--;
    er_scratch_end();

    /* Restore routing: prefer the next outer opacity slot, then the transform base, then NULL. */
    if (s_depth > 0)
    {
        er_scratch_begin(
            s_pool[s_depth - 1], ERUI_SCRATCH_W, ERUI_SCRATCH_H, s_meta[s_depth - 1].ox, s_meta[s_depth - 1].oy);
    }
    else if (s_base_buf)
    {
        er_scratch_begin(s_base_buf, s_base_w, s_base_h, s_base_ox, s_base_oy);
    }

    const uint32_t* slot = s_pool[s_depth];
    const int stride = (int)(ERUI_SCRATCH_W * sizeof(uint32_t));
    er_blit_blend(slot, stride, alpha, x, y, w, h);
}
