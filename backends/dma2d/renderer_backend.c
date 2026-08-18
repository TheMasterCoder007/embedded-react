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

#include "dma2d_backend.h"

#include "native_renderer.h"

#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief DMA2D register map — identical on every STM32 family that ships the peripheral
 *        (F4/F7/H7/U5, RM0090/RM0385/RM0433). Only the base address differs, and the caller
 *        supplies that, so no vendor header is needed.
 */
typedef struct
{
    volatile uint32_t CR;      /**< 0x00 control: MODE[17:16], TCIE, TEIE, START. */
    volatile uint32_t ISR;     /**< 0x04 status: CEIF, CTCIF, CAEIF, TWIF, TCIF, TEIF. */
    volatile uint32_t IFCR;    /**< 0x08 interrupt flag clear. */
    volatile uint32_t FGMAR;   /**< 0x0C foreground memory address. */
    volatile uint32_t FGOR;    /**< 0x10 foreground line offset (pixels). */
    volatile uint32_t BGMAR;   /**< 0x14 background memory address. */
    volatile uint32_t BGOR;    /**< 0x18 background line offset (pixels). */
    volatile uint32_t FGPFCCR; /**< 0x1C foreground PFC control (color mode, alpha mode). */
    volatile uint32_t FGCOLR;  /**< 0x20 foreground color. */
    volatile uint32_t BGPFCCR; /**< 0x24 background PFC control. */
    volatile uint32_t BGCOLR;  /**< 0x28 background color. */
    volatile uint32_t FGCMAR;  /**< 0x2C foreground CLUT address. */
    volatile uint32_t BGCMAR;  /**< 0x30 background CLUT address. */
    volatile uint32_t OPFCCR;  /**< 0x34 output PFC control (color mode). */
    volatile uint32_t OCOLR;   /**< 0x38 output color (R2M fill value, in output format). */
    volatile uint32_t OMAR;    /**< 0x3C output memory address. */
    volatile uint32_t OOR;     /**< 0x40 output line offset (pixels). */
    volatile uint32_t NLR;     /**< 0x44 number of lines: PL[29:16], NL[15:0]. */
    volatile uint32_t LWR;     /**< 0x48 line watermark. */
    volatile uint32_t AMTCR;   /**< 0x4C AHB master timer (dead time). */
} ErDma2dRegs;

/** @brief Internal state for the DMA2D render backend. */
typedef struct
{
    ErDma2dBackendConfig cfg;
    ErDma2dRegs* regs;
    uint8_t* fb;
    int bpp;       /**< Bytes per framebuffer pixel. */
    int stride_px; /**< Row pitch in pixels. */
    bool pending;  /**< A DMA2D transfer was started and not yet waited on. */
    int dx0, dy0;  /**< Dirty box (inclusive min corner). */
    int dx1, dy1;  /**< Dirty box (exclusive max corner); dx1 <= dx0 means empty. */
} Dma2dCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Constants: Private
 ---------------------------------------------------------------------------------------------------------------------*/

#define DMA2D_CR_START 0x00000001U
#define DMA2D_CR_MODE_M2M 0x00000000U     /* memory-to-memory, no PFC */
#define DMA2D_CR_MODE_M2M_PFC 0x00010000U /* memory-to-memory with pixel-format conversion */
#define DMA2D_CR_MODE_R2M 0x00030000U     /* register-to-memory fill */
#define DMA2D_ISR_TCIF 0x00000002U
#define DMA2D_ISR_TEIF 0x00000001U
#define DMA2D_ISR_CEIF 0x00000020U
#define DMA2D_ISR_DONE (DMA2D_ISR_TCIF | DMA2D_ISR_TEIF | DMA2D_ISR_CEIF)
#define DMA2D_IFCR_ALL 0x0000003FU
#define DMA2D_AMTCR_EN 0x00000001U

#define DMA2D_CM_ARGB8888 0x00000000U
#define DMA2D_CM_RGB888 0x00000001U
#define DMA2D_CM_RGB565 0x00000002U

/** @brief Default floor below which an op is cheaper on the CPU than programming the peripheral. */
#define DMA2D_MIN_DMA_PIXELS_DEFAULT 64

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static Dma2dCtx s_ctx;
static EmbeddedRenderBackend s_backend;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (pixel helpers)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Rounds (x / 255) to nearest for x in [0, 65025], without a divide. */
static inline uint32_t div255(uint32_t x)
{
    x += 0x80U;
    return (x + (x >> 8)) >> 8;
}

/** @brief Loads one framebuffer pixel as opaque ARGB8888, whatever the framebuffer format. */
static inline uint32_t fb_load(const uint8_t* p, int bpp)
{
    switch (bpp)
    {
        case 2:
        {
            const uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
            const uint32_t r = (v >> 11) & 0x1FU;
            const uint32_t g = (v >> 5) & 0x3FU;
            const uint32_t b = v & 0x1FU;
            /* expand with bit replication so round-trips are exact */
            return 0xFF000000U | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
        }
        case 3:
            return 0xFF000000U | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
        default:
            return ((const uint32_t*)(const void*)p)[0] | 0xFF000000U;
    }
}

/** @brief Stores an ARGB8888 value into one framebuffer pixel, whatever the framebuffer format. */
static inline void fb_store(uint8_t* p, int bpp, uint32_t argb)
{
    switch (bpp)
    {
        case 2:
        {
            const uint32_t r = (argb >> 16) & 0xFFU;
            const uint32_t g = (argb >> 8) & 0xFFU;
            const uint32_t b = argb & 0xFFU;
            const uint32_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            p[0] = (uint8_t)v;
            p[1] = (uint8_t)(v >> 8);
            break;
        }
        case 3:
            p[0] = (uint8_t)argb;         /* B */
            p[1] = (uint8_t)(argb >> 8);  /* G */
            p[2] = (uint8_t)(argb >> 16); /* R */
            break;
        default:
            ((uint32_t*)(void*)p)[0] = 0xFF000000U | (argb & 0x00FFFFFFU);
            break;
    }
}

/**
 * @brief Composites one premultiplied-ARGB source pixel over an opaque destination pixel (Porter-Duff
 *        source-over). The framebuffer is opaque, so only color channels blend.
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

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (state helpers)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Grows the dirty bounding box to include a written rectangle. */
static void mark_dirty(int x, int y, int w, int h)
{
    if (s_ctx.dx1 <= s_ctx.dx0)
    {
        s_ctx.dx0 = x;
        s_ctx.dy0 = y;
        s_ctx.dx1 = x + w;
        s_ctx.dy1 = y + h;
        return;
    }
    if (x < s_ctx.dx0)
        s_ctx.dx0 = x;
    if (y < s_ctx.dy0)
        s_ctx.dy0 = y;
    if (x + w > s_ctx.dx1)
        s_ctx.dx1 = x + w;
    if (y + h > s_ctx.dy1)
        s_ctx.dy1 = y + h;
}

/** @brief Clips a rectangle to the framebuffer, advancing the source origin to match. */
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
    if (cx + cw > s_ctx.cfg.width)
        cw = s_ctx.cfg.width - cx;
    if (cy + ch > s_ctx.cfg.height)
        ch = s_ctx.cfg.height - cy;
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

/** @brief Blocks until the in-flight DMA2D transfer (if any) has completed. */
static void wait_pending(void)
{
    if (!s_ctx.pending)
        return;

    if (s_ctx.cfg.wait_complete)
    {
        s_ctx.cfg.wait_complete(s_ctx.cfg.user);
    }
    else
    {
        while ((s_ctx.regs->ISR & DMA2D_ISR_DONE) == 0U)
        {
        }
        s_ctx.regs->IFCR = DMA2D_IFCR_ALL;
    }

    s_ctx.pending = false;
}

/** @brief Address of framebuffer pixel (x, y). */
static inline uint8_t* fb_at(int x, int y)
{
    return s_ctx.fb + ((size_t)y * (size_t)s_ctx.stride_px + (size_t)x) * (size_t)s_ctx.bpp;
}

/** @brief Cleans CPU-written lines the DMA2D is about to read (rows [addr, addr + span)). */
static void cache_clean_rows(const void* addr, size_t bytes)
{
    if (s_ctx.cfg.cache_clean)
        s_ctx.cfg.cache_clean(addr, bytes);
}

/** @brief Cleans+invalidates framebuffer rows the DMA2D is about to overwrite. */
static void cache_prepare_dst(int x, int y, int w, int h)
{
    if (!s_ctx.cfg.cache_clean_invalidate)
        return;
    uint8_t* first = fb_at(x, y);
    uint8_t* last = fb_at(x + w, y + h - 1);
    s_ctx.cfg.cache_clean_invalidate(first, (size_t)(last - first));
}

/**
 * @brief Programs everything but START for one transfer into the framebuffer, then launches it.
 *
 * The previous transfer is always waited on first — DMA2D has a single transfer context.
 */
static void launch(uint32_t mode)
{
    if (s_ctx.cfg.start)
    {
        s_ctx.cfg.start(s_ctx.cfg.user); /* owner sets START (typically with TC/TE interrupts) */
    }
    else
    {
        s_ctx.regs->IFCR = DMA2D_IFCR_ALL;
        s_ctx.regs->CR = mode | DMA2D_CR_START;
    }
    s_ctx.pending = true;
}

/** @brief Packs a straight-alpha ARGB color into the R2M output-color register for the fb format. */
static uint32_t pack_ocolr(uint32_t argb)
{
    switch (s_ctx.cfg.format)
    {
        case ER_DMA2D_FB_RGB565:
        {
            const uint32_t r = (argb >> 16) & 0xFFU;
            const uint32_t g = (argb >> 8) & 0xFFU;
            const uint32_t b = argb & 0xFFU;
            return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }
        case ER_DMA2D_FB_RGB888:
            return argb & 0x00FFFFFFU;
        default:
            return 0xFF000000U | (argb & 0x00FFFFFFU);
    }
}

/** @brief True when every pixel of a premultiplied-ARGB region has alpha 0xFF. */
static bool region_is_opaque(const uint8_t* base, int stride_bytes, int sx, int sy, int w, int h)
{
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)(const void*)(base + (size_t)(sy + row) * (size_t)stride_bytes) + sx;
        uint32_t acc = 0xFF000000U;
        for (int col = 0; col < w; col++)
            acc &= s[col];
        if ((acc & 0xFF000000U) != 0xFF000000U)
            return false;
    }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (CPU fallback paths)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief CPU opaque fill. */
static void cpu_fill_opaque(uint32_t argb, int x, int y, int w, int h)
{
    for (int row = 0; row < h; row++)
    {
        uint8_t* d = fb_at(x, y + row);
        for (int col = 0; col < w; col++, d += s_ctx.bpp)
            fb_store(d, s_ctx.bpp, argb);
    }
}

/** @brief CPU translucent fill (straight alpha, premultiplied once then source-over). */
static void cpu_fill_blend(uint32_t argb, uint32_t a, int x, int y, int w, int h)
{
    const uint32_t sr = div255(((argb >> 16) & 0xFFU) * a);
    const uint32_t sg = div255(((argb >> 8) & 0xFFU) * a);
    const uint32_t sb = div255((argb & 0xFFU) * a);
    for (int row = 0; row < h; row++)
    {
        uint8_t* d = fb_at(x, y + row);
        for (int col = 0; col < w; col++, d += s_ctx.bpp)
            fb_store(d, s_ctx.bpp, over_premul(fb_load(d, s_ctx.bpp), a, sr, sg, sb));
    }
}

/** @brief CPU copy/blend of a premultiplied ARGB8888 source at a global alpha (255 = plain copy). */
static void
cpu_copy_blend(const uint8_t* base, int stride_bytes, uint32_t ga, int sx, int sy, int x, int y, int w, int h)
{
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)(const void*)(base + (size_t)(sy + row) * (size_t)stride_bytes) + sx;
        uint8_t* d = fb_at(x, y + row);
        for (int col = 0; col < w; col++, d += s_ctx.bpp)
        {
            const uint32_t sp = s[col];
            uint32_t sa = (sp >> 24) & 0xFFU;
            uint32_t sr = (sp >> 16) & 0xFFU;
            uint32_t sg = (sp >> 8) & 0xFFU;
            uint32_t sb = sp & 0xFFU;
            if (ga != 255U)
            {
                sa = div255(sa * ga);
                sr = div255(sr * ga);
                sg = div255(sg * ga);
                sb = div255(sb * ga);
            }
            if (sa == 0U)
                continue;
            if (sa == 0xFFU)
                fb_store(d, s_ctx.bpp, 0xFF000000U | (sr << 16) | (sg << 8) | sb);
            else
                fb_store(d, s_ctx.bpp, over_premul(fb_load(d, s_ctx.bpp), sa, sr, sg, sb));
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (backend callbacks)
 ---------------------------------------------------------------------------------------------------------------------*/

static void fill_rect_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U || !clip_rect(&x, &y, &w, &h, NULL, NULL))
        return;

    mark_dirty(x, y, w, h);
    wait_pending(); /* single transfer context; also fences before any CPU access below */

    if (a == 0xFFU && (w * h) >= s_ctx.cfg.min_dma_pixels)
    {
        cache_prepare_dst(x, y, w, h);
        s_ctx.regs->CR = DMA2D_CR_MODE_R2M;
        s_ctx.regs->OPFCCR = (uint32_t)s_ctx.cfg.format;
        s_ctx.regs->OCOLR = pack_ocolr(argb);
        s_ctx.regs->OMAR = (uint32_t)(uintptr_t)fb_at(x, y);
        s_ctx.regs->OOR = (uint32_t)(s_ctx.stride_px - w);
        s_ctx.regs->NLR = ((uint32_t)w << 16) | (uint32_t)h;
        launch(DMA2D_CR_MODE_R2M);
        return;
    }

    if (a == 0xFFU)
        cpu_fill_opaque(argb, x, y, w, h);
    else
        cpu_fill_blend(argb, a, x, y, w, h);
}

static void copy_rect_cb(const void* src, int src_stride_bytes, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    int sx = 0, sy = 0;
    if (!src || !clip_rect(&x, &y, &w, &h, &sx, &sy))
        return;

    mark_dirty(x, y, w, h);
    wait_pending();

    const uint8_t* base = (const uint8_t*)src;
    const uint8_t* dma_first = base + (size_t)sy * (size_t)src_stride_bytes + (size_t)sx * 4U;

    /* An opaque source is a pure PFC copy the peripheral can do outright; scanning the alpha
       channel first is one read pass, far cheaper than the read-modify-write blend it avoids.
       The alignment gate matches copy_rect_fmt_cb's: a word-misaligned ARGB8888 FGMAR raises a
       DMA2D configuration error (transfer refused, image silently absent), so such a source must
       take the CPU path below instead. */
    if ((w * h) >= s_ctx.cfg.min_dma_pixels && (((uintptr_t)dma_first & 3U) == 0U) && ((src_stride_bytes % 4) == 0)
        && region_is_opaque(base, src_stride_bytes, sx, sy, w, h))
    {
        const uint8_t* first = dma_first;
        cache_clean_rows(first, (size_t)(h - 1) * (size_t)src_stride_bytes + (size_t)w * 4U);
        cache_prepare_dst(x, y, w, h);
        s_ctx.regs->CR = DMA2D_CR_MODE_M2M_PFC;
        s_ctx.regs->FGMAR = (uint32_t)(uintptr_t)first;
        s_ctx.regs->FGOR = (uint32_t)(src_stride_bytes / 4 - w);
        s_ctx.regs->FGPFCCR = DMA2D_CM_ARGB8888;
        s_ctx.regs->OPFCCR = (uint32_t)s_ctx.cfg.format;
        s_ctx.regs->OMAR = (uint32_t)(uintptr_t)fb_at(x, y);
        s_ctx.regs->OOR = (uint32_t)(s_ctx.stride_px - w);
        s_ctx.regs->NLR = ((uint32_t)w << 16) | (uint32_t)h;
        launch(DMA2D_CR_MODE_M2M_PFC);
        return;
    }

    /* Mixed-alpha source: DMA2D's blender expects straight-alpha foregrounds, but the engine
       hands premultiplied pixels — hardware blending would double-multiply. Composite on CPU. */
    cpu_copy_blend(base, src_stride_bytes, 255U, sx, sy, x, y, w, h);
}

/**
 * @brief Copies a KNOWN-FULLY-OPAQUE source in its own pixel format into the framebuffer (replace).
 *
 * This is the engine's format-aware entry point. Two things separate it from copy_rect_cb:
 *
 *  - The engine GUARANTEES every source pixel is opaque (the registry scans ARGB8888 pixels once at
 *    registration; RGB565 is opaque by construction), so the region_is_opaque() pre-scan is skipped.
 *    On a full-screen ARGB background that alone drops a width*height*4 byte read pass off every
 *    full repaint.
 *  - A non-ARGB source is handed to the DMA2D as-is and converted by the peripheral's PFC on the
 *    fly: FGPFCCR color mode RGB565 in, the framebuffer's mode out, ONE M2M_PFC transfer for the
 *    whole rect. Without this hook the engine expands 16-bit rows on the CPU and emits one blit per
 *    scanline — one conversion per pixel and one transfer per panel row.
 */
static void
copy_rect_fmt_cb(const void* src, int src_stride_bytes, ERImageFormat fmt, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    int sx = 0, sy = 0;
    if (!src || !clip_rect(&x, &y, &w, &h, &sx, &sy))
        return;

    mark_dirty(x, y, w, h);
    wait_pending();

    const int src_bpp = (fmt == ER_IMG_RGB565) ? 2 : 4;
    const uint8_t* first = (const uint8_t*)src + (size_t)sy * (size_t)src_stride_bytes + (size_t)sx * (size_t)src_bpp;

    /* The DMA2D REQUIRES the source address aligned to the color mode's pixel size: an FGMAR of
       4n+2 with CM=ARGB8888 raises a CONFIGURATION ERROR - the transfer never starts, ISR.CEIF
       is set, and the image is silently absent. Both formats are therefore gated here, and a
       misaligned source takes the CPU replace below, which is correct at any alignment. The asset
       packer 4-aligns every image's pixels, so this rarely trips, but the backend must stay
       correct against any caller. */
    const bool aligned = ((((uintptr_t)first) & (uintptr_t)(src_bpp - 1)) == 0U) && ((src_stride_bytes % src_bpp) == 0);

    if (aligned && (w * h) >= s_ctx.cfg.min_dma_pixels)
    {
        cache_clean_rows(first, (size_t)(h - 1) * (size_t)src_stride_bytes + (size_t)w * (size_t)src_bpp);
        cache_prepare_dst(x, y, w, h);
        s_ctx.regs->CR = DMA2D_CR_MODE_M2M_PFC;
        s_ctx.regs->FGMAR = (uint32_t)(uintptr_t)first;
        /* FGOR is the gap in PIXELS of the SOURCE format, not the destination's. */
        s_ctx.regs->FGOR = (uint32_t)(src_stride_bytes / src_bpp - w);
        s_ctx.regs->FGPFCCR = (fmt == ER_IMG_RGB565) ? DMA2D_CM_RGB565 : DMA2D_CM_ARGB8888;
        s_ctx.regs->OPFCCR = (uint32_t)s_ctx.cfg.format;
        s_ctx.regs->OMAR = (uint32_t)(uintptr_t)fb_at(x, y);
        s_ctx.regs->OOR = (uint32_t)(s_ctx.stride_px - w);
        s_ctx.regs->NLR = ((uint32_t)w << 16) | (uint32_t)h;
        launch(DMA2D_CR_MODE_M2M_PFC);
        return;
    }

    /* Below the floor where programming the peripheral pays for itself. Still a plain replace -
       the opacity guarantee means no destination read and no blend either way. */
    if (fmt == ER_IMG_RGB565)
    {
        for (int row = 0; row < h; row++)
        {
            const uint8_t* s = first + (size_t)row * (size_t)src_stride_bytes;
            uint8_t* d = fb_at(x, y + row);
            for (int col = 0; col < w; col++, d += s_ctx.bpp)
            {
                const uint32_t v = (uint32_t)s[col * 2] | ((uint32_t)s[col * 2 + 1] << 8);
                const uint32_t r = (v >> 11) & 0x1FU;
                const uint32_t g = (v >> 5) & 0x3FU;
                const uint32_t b = v & 0x1FU;
                /* Bit replication, matching the engine's own expansion exactly. */
                fb_store(d,
                         s_ctx.bpp,
                         0xFF000000U | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8)
                             | ((b << 3) | (b >> 2)));
            }
        }
        return;
    }

    cpu_copy_blend((const uint8_t*)src, src_stride_bytes, 255U, sx, sy, x, y, w, h);
}

static void blend_rect_cb(const void* src, int src_stride_bytes, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    if (alpha == 255U)
    {
        copy_rect_cb(src, src_stride_bytes, x, y, w, h, ctx);
        return;
    }

    int sx = 0, sy = 0;
    if (!src || alpha == 0U || !clip_rect(&x, &y, &w, &h, &sx, &sy))
        return;

    mark_dirty(x, y, w, h);
    wait_pending();
    cpu_copy_blend((const uint8_t*)src, src_stride_bytes, alpha, sx, sy, x, y, w, h);
}

static void wait_cb(void* ctx)
{
    (void)ctx;
    wait_pending();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_dma2d_backend_init(const ErDma2dBackendConfig* config)
{
    if (!config || !config->dma2d || !config->framebuffer || config->width <= 0 || config->height <= 0)
        return false;
    if (config->stride_pixels != 0 && config->stride_pixels < config->width)
        return false;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg = *config;
    s_ctx.regs = (ErDma2dRegs*)config->dma2d;
    s_ctx.fb = (uint8_t*)config->framebuffer;
    s_ctx.stride_px = config->stride_pixels > 0 ? config->stride_pixels : config->width;
    s_ctx.bpp = (config->format == ER_DMA2D_FB_RGB565) ? 2 : (config->format == ER_DMA2D_FB_RGB888) ? 3 : 4;
    if (s_ctx.cfg.min_dma_pixels <= 0)
        s_ctx.cfg.min_dma_pixels = DMA2D_MIN_DMA_PIXELS_DEFAULT;

    s_ctx.regs->AMTCR = config->dead_time > 0 ? ((config->dead_time << 8) | DMA2D_AMTCR_EN) : 0U;

    memset(&s_backend, 0, sizeof(s_backend));
    s_backend.fill_rect = fill_rect_cb;
    s_backend.copy_rect = copy_rect_cb;
    s_backend.copy_rect_fmt = copy_rect_fmt_cb;
    s_backend.blend_rect = blend_rect_cb;
    s_backend.wait = wait_cb;
    s_backend.ctx = &s_ctx;

    embedded_renderer_set_backend(&s_backend);
    return true;
}

void er_dma2d_backend_set_framebuffer(void* framebuffer)
{
    if (!framebuffer)
        return;
    wait_pending();
    s_ctx.fb = (uint8_t*)framebuffer;
}

void* er_dma2d_backend_framebuffer(void)
{
    return s_ctx.fb;
}

void er_dma2d_backend_wait(void)
{
    wait_pending();
}

bool er_dma2d_backend_take_dirty(int* x, int* y, int* w, int* h)
{
    if (s_ctx.dx1 <= s_ctx.dx0)
        return false;
    if (x)
        *x = s_ctx.dx0;
    if (y)
        *y = s_ctx.dy0;
    if (w)
        *w = s_ctx.dx1 - s_ctx.dx0;
    if (h)
        *h = s_ctx.dy1 - s_ctx.dy0;
    s_ctx.dx0 = s_ctx.dy0 = s_ctx.dx1 = s_ctx.dy1 = 0;
    return true;
}

void er_dma2d_backend_destroy(void)
{
    wait_pending();
    memset(&s_ctx, 0, sizeof(s_ctx));
}
