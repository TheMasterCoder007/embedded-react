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

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_scratch_push(int x, int y, int w, int h)
{
    if (s_depth >= ERUI_MAX_OPACITY_DEPTH)
        return false;
    if (w <= 0 || h <= 0 || w > ERUI_SCRATCH_W || h > ERUI_SCRATCH_H)
        return false;

    uint32_t* slot = s_pool[s_depth];
    memset(slot, 0, (size_t)ERUI_SCRATCH_W * (size_t)ERUI_SCRATCH_H * sizeof(uint32_t));
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

    /* If there is still an outer scratch slot, restore it as the active target so that
     * the blend below writes into the outer slot rather than the real framebuffer. */
    if (s_depth > 0)
    {
        er_scratch_begin(
            s_pool[s_depth - 1], ERUI_SCRATCH_W, ERUI_SCRATCH_H, s_meta[s_depth - 1].ox, s_meta[s_depth - 1].oy);
    }

    const uint32_t* slot = s_pool[s_depth];
    const int stride = (int)(ERUI_SCRATCH_W * sizeof(uint32_t));
    er_blit_blend(slot, stride, alpha, x, y, w, h);
}
