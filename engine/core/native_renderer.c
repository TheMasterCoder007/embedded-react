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

#include "native_renderer.h"
#include "er_scene.h"
#include "font_blob.h"
#include "font_registry.h"
#include "image_registry.h"
#include "renderer_internal.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

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

/* Banded rendering. The strip currently being rendered occupies screen rows [s_band_oy, s_band_oy +
 * s_band_h). This is applied at the BACKEND-EMIT boundary only — NOT via the clip stack — so it bounds
 * what lands in the band buffer without truncating offscreen-scratch (transform / opacity) source
 * rendering, which must always be complete or a transformed node split across a strip seam shows an
 * anti-aliasing gap. Each backend blit is clamped to these rows and its Y translated to the 0-origin
 * band buffer. s_band_h == 0 disables both (the classic full-framebuffer path). */
static int s_band_oy = 0;
static int s_band_h = 0;

/* Scratch redirect: when s_scratch_buf is non-NULL all blit calls write into it. */
static uint32_t* s_scratch_buf = NULL;
static int s_scratch_w = 0;
static int s_scratch_h = 0;
static int s_scratch_ox = 0;
static int s_scratch_oy = 0;

/* Inherited draw alpha: multiplied into every blit while < 255. Set by the compositor when a
 * translucent group cannot be composited through a scratch slot (pool exhausted) — each
 * primitive is then dimmed individually, which is exact wherever siblings don't overlap.
 * Offscreen captures (opacity strips, transform sources) reset it to 255 for the capture and
 * re-apply it when the captured buffer is blended out, so it is never applied twice. */
static uint8_t s_draw_alpha = 255U;

/* Row chunk used to emit fills through the blend path while s_draw_alpha < 255: backend
 * fill_rect semantics for translucent colors vary (some overwrite), blend_rect is uniform. */
#define ER_FILL_ROW_CHUNK 256
static uint32_t s_fill_row[ER_FILL_ROW_CHUNK];

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

/**
 * @brief Source-over blends a straight-alpha ARGB fill into the active scratch buffer.
 *
 * Premultiplies the color components before blending so the scratch buffer stays in
 * premultiplied ARGB8888 throughout the compositing pass.
 *
 * @param[in] argb  Straight-alpha ARGB8888 fill color.
 * @param[in] lx    Scratch-local left edge.
 * @param[in] ly    Scratch-local top edge.
 * @param[in] lw    Width in pixels.
 * @param[in] lh    Height in pixels.
 */
static void scratch_do_fill(uint32_t argb, int lx, int ly, int lw, int lh)
{
    const uint8_t sa = (uint8_t)((argb >> 24) & 0xFFU);
    if (sa == 0U)
        return;

    /* Clamp once so the pixel loops below run without per-pixel bounds checks. */
    const int x0 = lx < 0 ? 0 : lx;
    const int y0 = ly < 0 ? 0 : ly;
    const int x1 = (lx + lw) > s_scratch_w ? s_scratch_w : (lx + lw);
    const int y1 = (ly + lh) > s_scratch_h ? s_scratch_h : (ly + lh);
    if (x0 >= x1 || y0 >= y1)
        return;

    const uint8_t sr = (uint8_t)((uint32_t)((argb >> 16) & 0xFFU) * sa / 255U);
    const uint8_t sg = (uint8_t)((uint32_t)((argb >> 8) & 0xFFU) * sa / 255U);
    const uint8_t sb = (uint8_t)((uint32_t)(argb & 0xFFU) * sa / 255U);
    const uint8_t inv_sa = (uint8_t)(255U - sa);

    if (sa == 255U)
    {
        const uint32_t px = (0xFFU << 24) | ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | sb;
        for (int row = y0; row < y1; row++)
        {
            uint32_t* dst_row = s_scratch_buf + (size_t)row * s_scratch_w;
            for (int col = x0; col < x1; col++)
                dst_row[col] = px;
        }
        return;
    }

    for (int row = y0; row < y1; row++)
    {
        uint32_t* dst_row = s_scratch_buf + (size_t)row * s_scratch_w;
        for (int col = x0; col < x1; col++)
        {
            const uint32_t d = dst_row[col];
            const uint8_t oa = (uint8_t)(sa + (uint32_t)((d >> 24) & 0xFFU) * inv_sa / 255U);
            const uint8_t or_ = (uint8_t)(sr + (uint32_t)((d >> 16) & 0xFFU) * inv_sa / 255U);
            const uint8_t og = (uint8_t)(sg + (uint32_t)((d >> 8) & 0xFFU) * inv_sa / 255U);
            const uint8_t ob = (uint8_t)(sb + (uint32_t)(d & 0xFFU) * inv_sa / 255U);
            dst_row[col] = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
        }
    }
}

/**
 * @brief Source-over blends a premultiplied ARGB source into the scratch buffer at a global alpha.
 *
 * @param[in] src     Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] alpha   Global alpha scale (0 = transparent, 255 = opaque).
 * @param[in] lx      Scratch-local left edge.
 * @param[in] ly      Scratch-local top edge.
 * @param[in] lw      Width in pixels.
 * @param[in] lh      Height in pixels.
 */
static void scratch_do_blend(const void* src, int stride, uint8_t alpha, int lx, int ly, int lw, int lh)
{
    if (alpha == 0U)
        return;

    /* Clamp once (advancing the source pointer to match) so the pixel loops below run
     * without per-pixel bounds checks. */
    const int x0 = lx < 0 ? 0 : lx;
    const int y0 = ly < 0 ? 0 : ly;
    const int x1 = (lx + lw) > s_scratch_w ? s_scratch_w : (lx + lw);
    const int y1 = (ly + lh) > s_scratch_h ? s_scratch_h : (ly + lh);
    if (x0 >= x1 || y0 >= y1)
        return;
    src = (const uint8_t*)src + (size_t)(y0 - ly) * (size_t)stride + (size_t)(x0 - lx) * 4U;

    for (int row = y0; row < y1; row++)
    {
        const uint32_t* src_row = (const uint32_t*)((const uint8_t*)src + (size_t)(row - y0) * (size_t)stride);
        uint32_t* dst_row = s_scratch_buf + (size_t)row * s_scratch_w;

        for (int col = x0; col < x1; col++)
        {
            uint32_t sp = src_row[col - x0];
            if (alpha < 255U)
            {
                /* Scale all premultiplied channels by the global alpha. */
                sp = ((uint32_t)((sp >> 24) & 0xFFU) * alpha / 255U << 24)
                     | ((uint32_t)((sp >> 16) & 0xFFU) * alpha / 255U << 16)
                     | ((uint32_t)((sp >> 8) & 0xFFU) * alpha / 255U << 8) | (uint32_t)(sp & 0xFFU) * alpha / 255U;
            }

            const uint8_t sa = (uint8_t)((sp >> 24) & 0xFFU);
            if (sa == 0U)
                continue;

            if (sa == 255U)
            {
                dst_row[col] = sp;
            }
            else
            {
                const uint32_t d = dst_row[col];
                const uint8_t inv_sa = (uint8_t)(255U - sa);
                const uint8_t oa = (uint8_t)(sa + (uint32_t)((d >> 24) & 0xFFU) * inv_sa / 255U);
                const uint8_t or_ = (uint8_t)(((sp >> 16) & 0xFFU) + (uint32_t)((d >> 16) & 0xFFU) * inv_sa / 255U);
                const uint8_t og = (uint8_t)(((sp >> 8) & 0xFFU) + (uint32_t)((d >> 8) & 0xFFU) * inv_sa / 255U);
                const uint8_t ob = (uint8_t)((sp & 0xFFU) + (uint32_t)(d & 0xFFU) * inv_sa / 255U);
                dst_row[col] = ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
            }
        }
    }
}

/**
 * @brief Copies premultiplied ARGB pixels directly into the scratch buffer (opaque overwrite).
 *
 * @param[in] src     Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] lx      Scratch-local left edge.
 * @param[in] ly      Scratch-local top edge.
 * @param[in] lw      Width in pixels.
 * @param[in] lh      Height in pixels.
 */
static void scratch_do_copy(const void* src, int stride, int lx, int ly, int lw, int lh)
{
    /* Clamp once, then each row is a straight memcpy (opaque overwrite). */
    const int x0 = lx < 0 ? 0 : lx;
    const int y0 = ly < 0 ? 0 : ly;
    const int x1 = (lx + lw) > s_scratch_w ? s_scratch_w : (lx + lw);
    const int y1 = (ly + lh) > s_scratch_h ? s_scratch_h : (ly + lh);
    if (x0 >= x1 || y0 >= y1)
        return;
    src = (const uint8_t*)src + (size_t)(y0 - ly) * (size_t)stride + (size_t)(x0 - lx) * 4U;

    const size_t bytes = (size_t)(x1 - x0) * sizeof(uint32_t);
    for (int row = y0; row < y1; row++)
    {
        const uint32_t* src_row = (const uint32_t*)((const uint8_t*)src + (size_t)(row - y0) * (size_t)stride);
        memcpy(s_scratch_buf + (size_t)row * s_scratch_w + x0, src_row, bytes);
    }
}

const EmbeddedRenderBackend* er_backend(void)
{
    return g_backend;
}

void er_scratch_begin(uint32_t* buf, int w, int h, int ox, int oy)
{
    s_scratch_buf = buf;
    s_scratch_w = w;
    s_scratch_h = h;
    s_scratch_ox = ox;
    s_scratch_oy = oy;
}

void er_scratch_end(void)
{
    s_scratch_buf = NULL;
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

void er_push_clip_reset(void)
{
    if (s_clip_depth >= ER_CLIP_STACK_DEPTH)
        return;
    /* Full-extent entry pushed WITHOUT intersecting the current top: used by offscreen source
     * captures (transform subtree render) that must see the whole subtree regardless of outer
     * scissors — the captured OUTPUT is clipped by the restored stack when it is blended out. */
    ClipEntry entry = {-(1 << 20), -(1 << 20), (1 << 21), (1 << 21)};
    s_clip_stack[s_clip_depth++] = entry;
}

void er_set_band(int oy, int h)
{
    s_band_oy = oy;
    s_band_h = h;
}

bool er_band_active(int* oy, int* h)
{
    if (oy)
        *oy = s_band_oy;
    if (h)
        *h = s_band_h;
    return s_band_h > 0;
}

bool er_get_clip_rect(int* x, int* y, int* w, int* h)
{
    if (s_clip_depth == 0)
        return false;
    const ClipEntry* c = &s_clip_stack[s_clip_depth - 1];
    if (x)
        *x = c->x;
    if (y)
        *y = c->y;
    if (w)
        *w = c->w;
    if (h)
        *h = c->h;
    return true;
}

void er_set_draw_alpha(uint8_t alpha)
{
    s_draw_alpha = alpha;
}

uint8_t er_get_draw_alpha(void)
{
    return s_draw_alpha;
}

void er_blit_fill(uint32_t argb, int x, int y, int w, int h)
{
    if (s_draw_alpha < 255U)
    {
        /* Inherited-alpha fallback: emit the fill through the blend path so the color is
         * composited over existing pixels on every backend. er_blit_blend applies
         * s_draw_alpha itself, so the row holds the color at its ORIGINAL alpha. */
        const uint8_t a = (uint8_t)((argb >> 24) & 0xFFU);
        if (a == 0U)
            return;
        if (!apply_clip(&x, &y, &w, &h))
            return;
        const uint32_t pr = (uint32_t)((argb >> 16) & 0xFFU) * a / 255U;
        const uint32_t pg = (uint32_t)((argb >> 8) & 0xFFU) * a / 255U;
        const uint32_t pb = (uint32_t)(argb & 0xFFU) * a / 255U;
        const uint32_t premul = ((uint32_t)a << 24) | (pr << 16) | (pg << 8) | pb;
        const int chunk = w < ER_FILL_ROW_CHUNK ? w : ER_FILL_ROW_CHUNK;
        for (int i = 0; i < chunk; i++)
            s_fill_row[i] = premul;
        for (int row = 0; row < h; row++)
            for (int cx = 0; cx < w; cx += ER_FILL_ROW_CHUNK)
            {
                const int cw = (w - cx) < ER_FILL_ROW_CHUNK ? (w - cx) : ER_FILL_ROW_CHUNK;
                er_blit_blend(s_fill_row, cw * (int)sizeof(uint32_t), 255U, x + cx, y + row, cw, 1);
            }
        return;
    }
    if (!apply_clip(&x, &y, &w, &h))
        return;
    if (s_scratch_buf)
    {
        scratch_do_fill(argb, x - s_scratch_ox, y - s_scratch_oy, w, h);
        return;
    }
    if (g_backend && g_backend->fill_rect)
    {
        if (s_band_h > 0)
        {
            const int top = s_band_oy, bot = s_band_oy + s_band_h;
            if (y < top)
            {
                h -= top - y;
                y = top;
            }
            if (y + h > bot)
                h = bot - y;
            if (h <= 0)
                return;
        }
        g_backend->fill_rect(argb, x, y - s_band_oy, w, h, g_backend->ctx);
    }
}

void er_blit_copy(const void* src, int stride, int x, int y, int w, int h)
{
    if (s_draw_alpha < 255U)
    {
        /* Inherited-alpha fallback: an opaque copy becomes a blend at the inherited alpha
         * (er_blit_blend applies s_draw_alpha itself). */
        er_blit_blend(src, stride, 255U, x, y, w, h);
        return;
    }
    const int orig_x = x;
    const int orig_y = y;
    if (!apply_clip(&x, &y, &w, &h))
        return;
    /* Advance source pointer to account for clipped rows and columns. */
    const int dx = x - orig_x;
    const int dy = y - orig_y;
    src = (const uint8_t*)src + (size_t)dy * (size_t)stride + (size_t)dx * 4U;
    if (s_scratch_buf)
    {
        scratch_do_copy(src, stride, x - s_scratch_ox, y - s_scratch_oy, w, h);
        return;
    }
    if (g_backend && g_backend->copy_rect)
    {
        if (s_band_h > 0)
        {
            const int top = s_band_oy, bot = s_band_oy + s_band_h;
            if (y < top)
            {
                const int d = top - y;
                src = (const uint8_t*)src + (size_t)d * (size_t)stride;
                h -= d;
                y = top;
            }
            if (y + h > bot)
                h = bot - y;
            if (h <= 0)
                return;
        }
        g_backend->copy_rect(src, stride, x, y - s_band_oy, w, h, g_backend->ctx);
    }
}

void er_blit_blend(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h)
{
    if (s_draw_alpha < 255U)
    {
        alpha = (uint8_t)((uint32_t)alpha * s_draw_alpha / 255U);
        if (alpha == 0U)
            return;
    }
    const int orig_x = x;
    const int orig_y = y;
    if (!apply_clip(&x, &y, &w, &h))
        return;
    const int dx = x - orig_x;
    const int dy = y - orig_y;
    src = (const uint8_t*)src + (size_t)dy * (size_t)stride + (size_t)dx * 4U;
    if (s_scratch_buf)
    {
        scratch_do_blend(src, stride, alpha, x - s_scratch_ox, y - s_scratch_oy, w, h);
        return;
    }
    if (g_backend && g_backend->blend_rect)
    {
        if (s_band_h > 0)
        {
            const int top = s_band_oy, bot = s_band_oy + s_band_h;
            if (y < top)
            {
                const int d = top - y;
                src = (const uint8_t*)src + (size_t)d * (size_t)stride;
                h -= d;
                y = top;
            }
            if (y + h > bot)
                h = bot - y;
            if (h <= 0)
                return;
        }
        g_backend->blend_rect(src, stride, alpha, x, y - s_band_oy, w, h, g_backend->ctx);
    }
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
    er_force_full_repaint(); /* fresh framebuffer: first commit must fully repaint, not damage-clip */
}

void embedded_renderer_tick(uint32_t delta_ms)
{
    er_anim_tick(delta_ms);
    er_layout_anim_tick(delta_ms);
    er_input_tick(delta_ms);
    er_tick(delta_ms);
}

void embedded_renderer_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y)
{
    er_dispatch_touch(finger_id, phase, x, y);
}

void embedded_renderer_key(uint32_t keycode, const char* utf8_char)
{
    er_text_input_key(keycode, utf8_char);
}
