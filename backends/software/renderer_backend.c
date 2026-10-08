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

#include "software_backend.h"

#include "native_renderer.h"

#include <stdlib.h>
#include <string.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Internal state for the software render backend.
 */
typedef struct
{
    uint32_t* fb; /**< ARGB8888 framebuffer (0xAARRGGBB), fb_w * fb_h pixels, row-major. */
    int fb_w;     /**< Framebuffer width in pixels. */
    int fb_h;     /**< Framebuffer height in pixels. */
} SoftCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static SoftCtx s_ctx;
static EmbeddedRenderBackend s_backend;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Rounds (x / 255) to nearest for x in [0, 65025], without a divide. */
static inline uint32_t div255(uint32_t x)
{
    x += 0x80U;
    return (x + (x >> 8)) >> 8;
}

/**
 * @brief Composites one premultiplied-ARGB source pixel over an opaque destination pixel (Porter-Duff
 *        source-over).
 *
 * The framebuffer is kept opaque, so only the color channels are blended and the result alpha stays 0xFF.
 *
 * @param[in] dst  Destination pixel (opaque ARGB8888).
 * @param[in] sa   Source alpha (premultiply coverage).
 * @param[in] sr   Premultiplied source red.
 * @param[in] sg   Premultiplied source green.
 * @param[in] sb   Premultiplied source blue.
 *
 * @return The blended opaque ARGB8888 pixel.
 */
static inline uint32_t over_premul(uint32_t dst, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb)
{
    const uint32_t inv = 255U - sa;
    const uint32_t dr = (dst >> 16) & 0xFFU;
    const uint32_t dg = (dst >> 8) & 0xFFU;
    const uint32_t db = dst & 0xFFU;
    const uint32_t r = sr + div255(dr * inv);
    const uint32_t g = sg + div255(dg * inv);
    const uint32_t b = sb + div255(db * inv);
    return 0xFF000000U | (r << 16) | (g << 8) | b;
}

#if defined(__AVX2__)
/*
 * AVX2 rows: eight pixels at a time, with the same integer arithmetic as over_premul() and div255(), so
 * every pixel matches the scalar path bit for bit. Columns past the last multiple of eight use the scalar
 * code. The scalar path is the reference; it is what non-AVX2 builds run.
 */

/** @brief Each pixel's alpha byte copied into the low byte of that pixel's four 16-bit channels. */
static inline __m256i alpha16(__m256i px, bool high)
{
    const __m256i lo = _mm256_setr_epi8(
        3, -1, 3, -1, 3, -1, 3, -1, 7, -1, 7, -1, 7, -1, 7, -1, 3, -1, 3, -1, 3, -1, 3, -1, 7, -1, 7, -1, 7, -1, 7, -1);
    const __m256i hi = _mm256_setr_epi8(11,
                                        -1,
                                        11,
                                        -1,
                                        11,
                                        -1,
                                        11,
                                        -1,
                                        15,
                                        -1,
                                        15,
                                        -1,
                                        15,
                                        -1,
                                        15,
                                        -1,
                                        11,
                                        -1,
                                        11,
                                        -1,
                                        11,
                                        -1,
                                        11,
                                        -1,
                                        15,
                                        -1,
                                        15,
                                        -1,
                                        15,
                                        -1,
                                        15,
                                        -1);
    return _mm256_shuffle_epi8(px, high ? hi : lo);
}

/** @brief div255() on sixteen 16-bit lanes (inputs up to 255 * 255). */
static inline __m256i div255_16(__m256i x)
{
    x = _mm256_add_epi16(x, _mm256_set1_epi16(0x80));
    return _mm256_srli_epi16(_mm256_add_epi16(x, _mm256_srli_epi16(x, 8)), 8);
}

/** @brief Each premultiplied channel of `px` times the 16-bit factors in `f` (lo/hi halves), divided by 255. */
static inline __m256i scale8(__m256i px, __m256i f_lo, __m256i f_hi)
{
    const __m256i zero = _mm256_setzero_si256();
    const __m256i lo = div255_16(_mm256_mullo_epi16(_mm256_unpacklo_epi8(px, zero), f_lo));
    const __m256i hi = div255_16(_mm256_mullo_epi16(_mm256_unpackhi_epi8(px, zero), f_hi));
    return _mm256_packus_epi16(lo, hi);
}

/** @brief over_premul() of eight premultiplied sources onto eight destinations; alpha comes out 0xFF. */
static inline __m256i over8(__m256i s, __m256i d)
{
    const __m256i full = _mm256_set1_epi16(255);
    const __m256i inv_lo = _mm256_sub_epi16(full, alpha16(s, false));
    const __m256i inv_hi = _mm256_sub_epi16(full, alpha16(s, true));
    return _mm256_or_si256(_mm256_add_epi8(s, scale8(d, inv_lo, inv_hi)), _mm256_set1_epi32((int)0xFF000000U));
}

/** @brief copy_rect_cb()'s row: opaque sources replace, transparent ones keep the destination. */
static int copy_row8(uint32_t* d, const uint32_t* s, int w)
{
    const __m256i alpha_mask = _mm256_set1_epi32((int)0xFF000000U);
    int col = 0;
    for (; col + 8 <= w; col += 8)
    {
        const __m256i sp = _mm256_loadu_si256((const __m256i*)(s + col));
        const __m256i a = _mm256_and_si256(sp, alpha_mask);
        const __m256i opaque = _mm256_cmpeq_epi32(a, alpha_mask);
        if (_mm256_movemask_epi8(opaque) == -1)
        {
            _mm256_storeu_si256((__m256i*)(d + col), sp);
            continue;
        }
        const __m256i clear = _mm256_cmpeq_epi32(a, _mm256_setzero_si256());
        if (_mm256_movemask_epi8(clear) == -1)
            continue;
        const __m256i dp = _mm256_loadu_si256((const __m256i*)(d + col));
        __m256i out = _mm256_blendv_epi8(over8(sp, dp), sp, opaque);
        out = _mm256_blendv_epi8(out, dp, clear);
        _mm256_storeu_si256((__m256i*)(d + col), out);
    }
    return col;
}
#endif

/**
 * @brief Clips a rectangle to the framebuffer bounds, advancing the source origin to match.
 *
 * @param[in,out] x   Left edge (screen space); clamped on return.
 * @param[in,out] y   Top edge (screen space); clamped on return.
 * @param[in,out] w   Width; reduced on return.
 * @param[in,out] h   Height; reduced on return.
 * @param[out]    sx  Source column offset introduced by left/top clipping (NULL to ignore).
 * @param[out]    sy  Source row offset introduced by left/top clipping (NULL to ignore).
 *
 * @return true if any pixels remain after clipping; false if fully off-screen.
 */
static bool clip_rect(int* x, int* y, int* w, int* h, int* sx, int* sy)
{
    int cx = *x, cy = *y, cw = *w, ch = *h, ox = 0, oy = 0;

    if (cw <= 0 || ch <= 0)
        return false;
    if (cx < 0)
    {
        ox = -cx;
        cw += cx;
        cx = 0;
    }
    if (cy < 0)
    {
        oy = -cy;
        ch += cy;
        cy = 0;
    }
    if (cx + cw > s_ctx.fb_w)
        cw = s_ctx.fb_w - cx;
    if (cy + ch > s_ctx.fb_h)
        ch = s_ctx.fb_h - cy;
    if (cw <= 0 || ch <= 0)
        return false;

    *x = cx;
    *y = cy;
    *w = cw;
    *h = ch;
    if (sx)
        *sx = ox;
    if (sy)
        *sy = oy;
    return true;
}

/**
 * @brief Fills a solid-color rectangle (straight-alpha ARGB8888) into the framebuffer.
 *
 * Opaque fills are written directly; translucent fills are source-over composited.
 *
 * @param[in] argb  Straight-alpha ARGB8888 color.
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] ctx   Pointer to the SoftCtx.
 */
static void fill_rect_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    SoftCtx* c = ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U || !clip_rect(&x, &y, &w, &h, NULL, NULL))
        return;

    if (a == 0xFFU)
    {
        const uint32_t px = 0xFF000000U | (argb & 0x00FFFFFFU);
        for (int row = 0; row < h; row++)
        {
            uint32_t* d = c->fb + (size_t)(y + row) * c->fb_w + x;
            for (int col = 0; col < w; col++)
                d[col] = px;
        }
        return;
    }

    /* Translucent: premultiply the straight-alpha source once, then source-over each pixel. */
    const uint32_t sr = div255(((argb >> 16) & 0xFFU) * a);
    const uint32_t sg = div255(((argb >> 8) & 0xFFU) * a);
    const uint32_t sb = div255((argb & 0xFFU) * a);
#if defined(__AVX2__)
    const __m256i src = _mm256_set1_epi32((int)((a << 24) | (sr << 16) | (sg << 8) | sb));
#endif
    for (int row = 0; row < h; row++)
    {
        uint32_t* d = c->fb + (size_t)(y + row) * c->fb_w + x;
        int col = 0;
#if defined(__AVX2__)
        for (; col + 8 <= w; col += 8)
            _mm256_storeu_si256((__m256i*)(d + col), over8(src, _mm256_loadu_si256((const __m256i*)(d + col))));
#endif
        for (; col < w; col++)
            d[col] = over_premul(d[col], a, sr, sg, sb);
    }
}

/**
 * @brief Copies a premultiplied ARGB8888 buffer into the framebuffer (source-over).
 *
 * @param[in] src              Source pixel buffer (premultiplied ARGB8888).
 * @param[in] src_stride_bytes Row stride of src in bytes.
 * @param[in] x                Destination X coordinate.
 * @param[in] y                Destination Y coordinate.
 * @param[in] w                Width of the region in pixels.
 * @param[in] h                Height of the region in pixels.
 * @param[in] ctx              Pointer to the SoftCtx.
 */
static void copy_rect_cb(const void* src, int src_stride_bytes, int x, int y, int w, int h, void* ctx)
{
    SoftCtx* c = ctx;
    int sx = 0, sy = 0;
    if (!src || !clip_rect(&x, &y, &w, &h, &sx, &sy))
        return;

    const uint8_t* base = (const uint8_t*)src;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)(base + (size_t)(sy + row) * src_stride_bytes) + sx;
        uint32_t* d = c->fb + (size_t)(y + row) * c->fb_w + x;
#if defined(__AVX2__)
        for (int col = copy_row8(d, s, w); col < w; col++)
#else
        for (int col = 0; col < w; col++)
#endif
        {
            const uint32_t sp = s[col];
            const uint32_t sa = (sp >> 24) & 0xFFU;
            if (sa == 0xFFU)
                d[col] = sp; /* opaque source replaces the destination */
            else if (sa != 0U)
                d[col] = over_premul(d[col], sa, (sp >> 16) & 0xFFU, (sp >> 8) & 0xFFU, sp & 0xFFU);
        }
    }
}

/**
 * @brief Blends a premultiplied ARGB8888 buffer into the framebuffer at a global alpha.
 *
 * Each source channel (including alpha) is scaled by alpha/255, preserving the premultiplied invariant, then
 * source-over composited — matching the engine's blend formula used by the other backends.
 *
 * @param[in] src              Source pixel buffer (premultiplied ARGB8888).
 * @param[in] src_stride_bytes Row stride of src in bytes.
 * @param[in] alpha            Global opacity (0 = transparent, 255 = opaque).
 * @param[in] x                Destination X coordinate.
 * @param[in] y                Destination Y coordinate.
 * @param[in] w                Width of the region in pixels.
 * @param[in] h                Height of the region in pixels.
 * @param[in] ctx              Pointer to the SoftCtx.
 */
static void blend_rect_cb(const void* src, int src_stride_bytes, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    if (alpha == 255U)
    {
        /* Nothing to scale — by 255/255 is the identity — and copy_rect_cb is the same source-over
         * with an opaque-pixel shortcut on top. Worth the redirect because the engine's own row
         * emitters (the vector AA rows, gradients, shadows, transforms) all blend at 255: without it
         * every one of their pixels pays four multiply-and-divides that cannot change its value.
         * Runs BEFORE the clip so copy_rect_cb still gets the unclipped rect to offset its source by. */
        copy_rect_cb(src, src_stride_bytes, x, y, w, h, ctx);
        return;
    }

    SoftCtx* c = ctx;
    int sx = 0, sy = 0;
    const uint32_t ga = alpha;
    if (!src || ga == 0U || !clip_rect(&x, &y, &w, &h, &sx, &sy))
        return;

    const uint8_t* base = (const uint8_t*)src;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)(base + (size_t)(sy + row) * src_stride_bytes) + sx;
        uint32_t* d = c->fb + (size_t)(y + row) * c->fb_w + x;
        int col = 0;
#if defined(__AVX2__)
        const __m256i g = _mm256_set1_epi16((short)ga);
        for (; col + 8 <= w; col += 8)
        {
            const __m256i sp = scale8(_mm256_loadu_si256((const __m256i*)(s + col)), g, g);
            const __m256i dp = _mm256_loadu_si256((const __m256i*)(d + col));
            const __m256i clear =
                _mm256_cmpeq_epi32(_mm256_and_si256(sp, _mm256_set1_epi32((int)0xFF000000U)), _mm256_setzero_si256());
            _mm256_storeu_si256((__m256i*)(d + col), _mm256_blendv_epi8(over8(sp, dp), dp, clear));
        }
#endif
        for (; col < w; col++)
        {
            const uint32_t sp = s[col];
            const uint32_t sa = div255(((sp >> 24) & 0xFFU) * ga);
            if (sa == 0U)
                continue;
            const uint32_t sr = div255(((sp >> 16) & 0xFFU) * ga);
            const uint32_t sg = div255(((sp >> 8) & 0xFFU) * ga);
            const uint32_t sb = div255((sp & 0xFFU) * ga);
            d[col] = over_premul(d[col], sa, sr, sg, sb);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_software_backend_init(int fb_w, int fb_h)
{
    if (fb_w <= 0 || fb_h <= 0)
        return false;

    s_ctx.fb = malloc((size_t)fb_w * (size_t)fb_h * sizeof(uint32_t));
    if (!s_ctx.fb)
        return false;

    s_ctx.fb_w = fb_w;
    s_ctx.fb_h = fb_h;
    er_software_clear(0xFF000000U); /* opaque black, like a freshly-cleared panel */

    s_backend.fill_rect = fill_rect_cb;
    s_backend.copy_rect = copy_rect_cb;
    s_backend.blend_rect = blend_rect_cb;
    s_backend.wait = NULL;
    s_backend.frame_ready = NULL;
    s_backend.ctx = &s_ctx;
    s_backend.band_height = 0; /* full-framebuffer path */
    s_backend.band_begin = NULL;
    s_backend.band_flush = NULL;

    embedded_renderer_set_backend(&s_backend);
    return true;
}

void er_software_backend_destroy(void)
{
    free(s_ctx.fb);
    s_ctx.fb = NULL;
    s_ctx.fb_w = 0;
    s_ctx.fb_h = 0;
}

bool er_software_backend_resize(int fb_w, int fb_h)
{
    if (fb_w <= 0 || fb_h <= 0)
        return false;

    uint32_t* nf = realloc(s_ctx.fb, (size_t)fb_w * (size_t)fb_h * sizeof(uint32_t));
    if (!nf)
        return false; /* old framebuffer is left intact on failure */

    s_ctx.fb = nf;
    s_ctx.fb_w = fb_w;
    s_ctx.fb_h = fb_h;
    er_software_clear(0xFF000000U);
    return true;
}

void er_software_clear(uint32_t argb)
{
    if (!s_ctx.fb)
        return;
    const size_t n = (size_t)s_ctx.fb_w * (size_t)s_ctx.fb_h;
    for (size_t i = 0; i < n; i++)
        s_ctx.fb[i] = argb;
}

uint32_t* er_software_framebuffer(void)
{
    return s_ctx.fb;
}

int er_software_fb_width(void)
{
    return s_ctx.fb_w;
}

int er_software_fb_height(void)
{
    return s_ctx.fb_h;
}
