#include "native_renderer.h"
#include "font_blob.h"
#include "font_registry.h"
#include "image_registry.h"
#include "renderer_internal.h"
#include <stdbool.h>
#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maximum depth of the scissor clip rectangle stack. */
#define ER_CLIP_STACK_DEPTH 8

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One entry in the scissor clip rectangle stack.
 */
typedef struct
{
    int x; /**< Left edge. */
    int y; /**< Top edge. */
    int w; /**< Width in pixels. */
    int h; /**< Height in pixels. */
} ClipEntry;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static const EmbeddedRenderBackend* g_backend = NULL;
static ClipEntry s_clip_stack[ER_CLIP_STACK_DEPTH];
static int s_clip_depth = 0;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Clips a destination rectangle (in/out) to the top-most active scissor rectangle.
 *
 * Adjusts x, y, w, h in place.  Returns false when the clipped area is empty.
 *
 * @param[in,out] x   Left edge of the rectangle; updated to the clipped left edge.
 * @param[in,out] y   Top edge of the rectangle; updated to the clipped top edge.
 * @param[in,out] w   Width; updated to the clipped width (may become 0).
 * @param[in,out] h   Height; updated to the clipped height (may become 0).
 *
 * @return true if the clipped rectangle has a positive area; false if it is empty.
 */
static bool apply_clip(int* x, int* y, int* w, int* h)
{
    if (s_clip_depth == 0)
        return true;

    const ClipEntry* c = &s_clip_stack[s_clip_depth - 1];
    const int x2 = *x + *w;
    const int y2 = *y + *h;
    const int cx2 = c->x + c->w;
    const int cy2 = c->y + c->h;

    const int nx = *x > c->x ? *x : c->x;
    const int ny = *y > c->y ? *y : c->y;
    const int nx2 = x2 < cx2 ? x2 : cx2;
    const int ny2 = y2 < cy2 ? y2 : cy2;

    *x = nx;
    *y = ny;
    *w = nx2 - nx;
    *h = ny2 - ny;
    return *w > 0 && *h > 0;
}

const EmbeddedRenderBackend* er_backend(void)
{
    return g_backend;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_push_clip_rect(int x, int y, int w, int h)
{
    if (s_clip_depth >= ER_CLIP_STACK_DEPTH)
        return;

    ClipEntry entry = {x, y, w, h};

    if (s_clip_depth > 0)
    {
        /* Intersect the new rect with the current top-of-stack. */
        const ClipEntry* top = &s_clip_stack[s_clip_depth - 1];
        const int x2 = x + w;
        const int y2 = y + h;
        const int tx2 = top->x + top->w;
        const int ty2 = top->y + top->h;

        entry.x = x > top->x ? x : top->x;
        entry.y = y > top->y ? y : top->y;
        const int ex2 = x2 < tx2 ? x2 : tx2;
        const int ey2 = y2 < ty2 ? y2 : ty2;
        entry.w = ex2 - entry.x;
        entry.h = ey2 - entry.y;
        if (entry.w < 0)
            entry.w = 0;
        if (entry.h < 0)
            entry.h = 0;
    }

    s_clip_stack[s_clip_depth++] = entry;
}

void er_pop_clip_rect(void)
{
    if (s_clip_depth > 0)
        s_clip_depth--;
}

void er_blit_fill(uint32_t argb, int x, int y, int w, int h)
{
    if (!apply_clip(&x, &y, &w, &h))
        return;
    if (g_backend && g_backend->fill_rect)
        g_backend->fill_rect(argb, x, y, w, h, g_backend->ctx);
}

void er_blit_copy(const void* src, int stride, int x, int y, int w, int h)
{
    const int orig_x = x;
    const int orig_y = y;
    if (!apply_clip(&x, &y, &w, &h))
        return;
    /* Advance source pointer to account for clipped rows and columns. */
    const int dx = x - orig_x;
    const int dy = y - orig_y;
    src = (const uint8_t*)src + (size_t)dy * (size_t)stride + (size_t)dx * 4U;
    if (g_backend && g_backend->copy_rect)
        g_backend->copy_rect(src, stride, x, y, w, h, g_backend->ctx);
}

void er_blit_blend(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h)
{
    const int orig_x = x;
    const int orig_y = y;
    if (!apply_clip(&x, &y, &w, &h))
        return;
    const int dx = x - orig_x;
    const int dy = y - orig_y;
    src = (const uint8_t*)src + (size_t)dy * (size_t)stride + (size_t)dx * 4U;
    if (g_backend && g_backend->blend_rect)
        g_backend->blend_rect(src, stride, alpha, x, y, w, h, g_backend->ctx);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void embedded_renderer_set_backend(const EmbeddedRenderBackend* backend)
{
    g_backend = backend;
    s_clip_depth = 0;
    font_registry_init();
    font_blob_init(0);
    image_registry_init();
    er_input_reset();
}

void embedded_renderer_tick(uint32_t delta_ms)
{
    er_anim_tick(delta_ms);
    er_input_tick(delta_ms);
    er_tick(delta_ms);
}

void embedded_renderer_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y)
{
    er_dispatch_touch(finger_id, phase, x, y);
}
