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
    ErDma2dBackendConfig config;
    ErDma2dRegs* regs;
    uint8_t* framebuffer;
    int bytes_per_pixel;    /**< Bytes per framebuffer pixel. */
    int stride_pixels;      /**< Row pitch in pixels. */
    bool pending;           /**< A DMA2D transfer was started and not yet waited on. */
    int dirty_x0, dirty_y0; /**< Dirty box, inclusive min corner. */
    int dirty_x_end,
        dirty_y_end; /**< Dirty box, EXCLUSIVE max corner (matches ERRect); dirty_x_end <= dirty_x0 means empty. */
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
#define DMA2D_AMTCR_DT_SHIFT 8 /* dead time, AMTCR[15:8] */
#define DMA2D_NLR_PL_SHIFT 16  /* pixels per line, NLR[29:16] */

#define DMA2D_CM_ARGB8888 0x00000000U
#define DMA2D_CM_RGB888 0x00000001U
#define DMA2D_CM_RGB565 0x00000002U

/** @brief Bytes per pixel of each pixel format. */
#define RGB565_BYTES 2
#define RGB888_BYTES 3
#define ARGB8888_BYTES 4

/** @brief Full scale of an 8-bit color channel. */
#define CHANNEL_MAX 255U

/** @brief An alpha of this value is fully opaque. */
#define ALPHA_OPAQUE 255U

/** @brief The alpha byte of an opaque ARGB8888 pixel. */
#define ARGB_OPAQUE_ALPHA 0xFF000000U

/** @brief Added before the divide in div255() so it rounds to nearest. */
#define DIV255_ROUND 0x80U

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
static inline uint32_t div255(uint32_t value)
{
    value += DIV255_ROUND;
    return (value + (value >> 8)) >> 8;
}

/** @brief Loads one framebuffer pixel as opaque ARGB8888, whatever the framebuffer format. */
static inline uint32_t fb_load(const uint8_t* pixel, int bytes_per_pixel)
{
    switch (bytes_per_pixel)
    {
        case RGB565_BYTES:
        {
            const uint32_t rgb565 = (uint32_t)pixel[0] | ((uint32_t)pixel[1] << 8);
            const uint32_t red5 = (rgb565 >> 11) & 0x1FU;
            const uint32_t green6 = (rgb565 >> 5) & 0x3FU;
            const uint32_t blue5 = rgb565 & 0x1FU;
            /* expand with bit replication so round-trips are exact */
            return ARGB_OPAQUE_ALPHA | (((red5 << 3) | (red5 >> 2)) << 16) | (((green6 << 2) | (green6 >> 4)) << 8)
                   | ((blue5 << 3) | (blue5 >> 2));
        }
        case RGB888_BYTES:
            return ARGB_OPAQUE_ALPHA | ((uint32_t)pixel[2] << 16) | ((uint32_t)pixel[1] << 8) | (uint32_t)pixel[0];
        default:
            return ((const uint32_t*)(const void*)pixel)[0] | ARGB_OPAQUE_ALPHA;
    }
}

/** @brief Stores an ARGB8888 value into one framebuffer pixel, whatever the framebuffer format. */
static inline void fb_store(uint8_t* pixel, int bytes_per_pixel, uint32_t argb)
{
    switch (bytes_per_pixel)
    {
        case RGB565_BYTES:
        {
            const uint32_t red = (argb >> 16) & 0xFFU;
            const uint32_t green = (argb >> 8) & 0xFFU;
            const uint32_t blue = argb & 0xFFU;
            const uint32_t rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
            pixel[0] = (uint8_t)rgb565;
            pixel[1] = (uint8_t)(rgb565 >> 8);
            break;
        }
        case RGB888_BYTES:
            pixel[0] = (uint8_t)argb;         /* B */
            pixel[1] = (uint8_t)(argb >> 8);  /* G */
            pixel[2] = (uint8_t)(argb >> 16); /* R */
            break;
        default:
            ((uint32_t*)(void*)pixel)[0] = ARGB_OPAQUE_ALPHA | (argb & 0x00FFFFFFU);
            break;
    }
}

/**
 * @brief Composites one premultiplied-ARGB source pixel over an opaque destination pixel (Porter-Duff
 *        source-over). The framebuffer is opaque, so only color channels blend.
 */
static inline uint32_t over_premul(
    uint32_t destination, uint32_t source_alpha, uint32_t source_red, uint32_t source_green, uint32_t source_blue)
{
    const uint32_t inverse_alpha = CHANNEL_MAX - source_alpha;
    const uint32_t destination_red = (destination >> 16) & 0xFFU;
    const uint32_t destination_green = (destination >> 8) & 0xFFU;
    const uint32_t destination_blue = destination & 0xFFU;
    const uint32_t red = source_red + div255(destination_red * inverse_alpha);
    const uint32_t green = source_green + div255(destination_green * inverse_alpha);
    const uint32_t blue = source_blue + div255(destination_blue * inverse_alpha);
    return ARGB_OPAQUE_ALPHA | (red << 16) | (green << 8) | blue;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (state helpers)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Grows the dirty bounding box to include a written rectangle. */
static void mark_dirty(int x, int y, int width, int height)
{
    if (s_ctx.dirty_x_end <= s_ctx.dirty_x0)
    {
        s_ctx.dirty_x0 = x;
        s_ctx.dirty_y0 = y;
        s_ctx.dirty_x_end = x + width;
        s_ctx.dirty_y_end = y + height;
        return;
    }
    if (x < s_ctx.dirty_x0)
        s_ctx.dirty_x0 = x;
    if (y < s_ctx.dirty_y0)
        s_ctx.dirty_y0 = y;
    if (x + width > s_ctx.dirty_x_end)
        s_ctx.dirty_x_end = x + width;
    if (y + height > s_ctx.dirty_y_end)
        s_ctx.dirty_y_end = y + height;
}

/** @brief Clips a rectangle to the framebuffer, advancing the source origin to match. */
static bool clip_rect(int* x, int* y, int* width, int* height, int* source_x, int* source_y)
{
    int clipped_x = *x, clipped_y = *y, clipped_width = *width, clipped_height = *height, offset_x = 0, offset_y = 0;

    if (clipped_width <= 0 || clipped_height <= 0)
        return false;
    if (clipped_x < 0)
    {
        offset_x = -clipped_x;
        clipped_width += clipped_x;
        clipped_x = 0;
    }
    if (clipped_y < 0)
    {
        offset_y = -clipped_y;
        clipped_height += clipped_y;
        clipped_y = 0;
    }
    if (clipped_x + clipped_width > s_ctx.config.width)
        clipped_width = s_ctx.config.width - clipped_x;
    if (clipped_y + clipped_height > s_ctx.config.height)
        clipped_height = s_ctx.config.height - clipped_y;
    if (clipped_width <= 0 || clipped_height <= 0)
        return false;

    *x = clipped_x;
    *y = clipped_y;
    *width = clipped_width;
    *height = clipped_height;
    if (source_x)
        *source_x = offset_x;
    if (source_y)
        *source_y = offset_y;
    return true;
}

/** @brief Blocks until the in-flight DMA2D transfer (if any) has completed. */
static void wait_pending(void)
{
    if (!s_ctx.pending)
        return;

    if (s_ctx.config.wait_complete)
    {
        s_ctx.config.wait_complete(s_ctx.config.user);
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
    return s_ctx.framebuffer + ((size_t)y * (size_t)s_ctx.stride_pixels + (size_t)x) * (size_t)s_ctx.bytes_per_pixel;
}

/** @brief Cleans CPU-written lines the DMA2D is about to read (rows [addr, addr + span)). */
static void cache_clean_rows(const void* address, size_t bytes)
{
    if (s_ctx.config.cache_clean)
        s_ctx.config.cache_clean(address, bytes);
}

/** @brief Cleans+invalidates framebuffer rows the DMA2D is about to overwrite. */
static void cache_prepare_dst(int x, int y, int width, int height)
{
    if (!s_ctx.config.cache_clean_invalidate)
        return;
    uint8_t* first = fb_at(x, y);
    uint8_t* last = fb_at(x + width, y + height - 1);
    s_ctx.config.cache_clean_invalidate(first, (size_t)(last - first));
}

/**
 * @brief Programs everything but START for one transfer into the framebuffer, then launches it.
 *
 * The previous transfer is always waited on first — DMA2D has a single transfer context.
 */
static void launch(uint32_t mode)
{
    if (s_ctx.config.start)
    {
        s_ctx.config.start(s_ctx.config.user); /* owner sets START (typically with TC/TE interrupts) */
    }
    else
    {
        s_ctx.regs->IFCR = DMA2D_IFCR_ALL;
        s_ctx.regs->CR = mode | DMA2D_CR_START;
    }
    s_ctx.pending = true;
}

/** @brief Packs a straight-alpha ARGB color into the R2M output-color register for the framebuffer format. */
static uint32_t pack_ocolr(uint32_t argb)
{
    switch (s_ctx.config.format)
    {
        case ER_DMA2D_FB_RGB565:
        {
            const uint32_t red = (argb >> 16) & 0xFFU;
            const uint32_t green = (argb >> 8) & 0xFFU;
            const uint32_t blue = argb & 0xFFU;
            return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
        }
        case ER_DMA2D_FB_RGB888:
            return argb & 0x00FFFFFFU;
        default:
            return ARGB_OPAQUE_ALPHA | (argb & 0x00FFFFFFU);
    }
}

/** @brief True when every pixel of a premultiplied-ARGB region has alpha 0xFF. */
static bool
region_is_opaque(const uint8_t* source_bytes, int stride_bytes, int source_x, int source_y, int width, int height)
{
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row =
            (const uint32_t*)(const void*)(source_bytes + (size_t)(source_y + row) * (size_t)stride_bytes) + source_x;
        uint32_t combined = ARGB_OPAQUE_ALPHA;
        for (int column = 0; column < width; column++)
            combined &= source_row[column];
        if ((combined & ARGB_OPAQUE_ALPHA) != ARGB_OPAQUE_ALPHA)
            return false;
    }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (CPU fallback paths)
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief CPU opaque fill. */
static void cpu_fill_opaque(uint32_t argb, int x, int y, int width, int height)
{
    for (int row = 0; row < height; row++)
    {
        uint8_t* destination = fb_at(x, y + row);
        for (int column = 0; column < width; column++, destination += s_ctx.bytes_per_pixel)
            fb_store(destination, s_ctx.bytes_per_pixel, argb);
    }
}

/** @brief CPU translucent fill (straight alpha, premultiplied once then source-over). */
static void cpu_fill_blend(uint32_t argb, uint32_t alpha, int x, int y, int width, int height)
{
    const uint32_t source_red = div255(((argb >> 16) & 0xFFU) * alpha);
    const uint32_t source_green = div255(((argb >> 8) & 0xFFU) * alpha);
    const uint32_t source_blue = div255((argb & 0xFFU) * alpha);
    for (int row = 0; row < height; row++)
    {
        uint8_t* destination = fb_at(x, y + row);
        for (int column = 0; column < width; column++, destination += s_ctx.bytes_per_pixel)
            fb_store(
                destination,
                s_ctx.bytes_per_pixel,
                over_premul(fb_load(destination, s_ctx.bytes_per_pixel), alpha, source_red, source_green, source_blue));
    }
}

/** @brief CPU copy/blend of a premultiplied ARGB8888 source at a global alpha (255 = plain copy). */
static void cpu_copy_blend(const uint8_t* source_bytes,
                           int stride_bytes,
                           uint32_t global_alpha,
                           int source_x,
                           int source_y,
                           int x,
                           int y,
                           int width,
                           int height)
{
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row =
            (const uint32_t*)(const void*)(source_bytes + (size_t)(source_y + row) * (size_t)stride_bytes) + source_x;
        uint8_t* destination = fb_at(x, y + row);
        for (int column = 0; column < width; column++, destination += s_ctx.bytes_per_pixel)
        {
            const uint32_t source_pixel = source_row[column];
            uint32_t source_alpha = (source_pixel >> 24) & 0xFFU;
            uint32_t source_red = (source_pixel >> 16) & 0xFFU;
            uint32_t source_green = (source_pixel >> 8) & 0xFFU;
            uint32_t source_blue = source_pixel & 0xFFU;
            if (global_alpha != ALPHA_OPAQUE)
            {
                source_alpha = div255(source_alpha * global_alpha);
                source_red = div255(source_red * global_alpha);
                source_green = div255(source_green * global_alpha);
                source_blue = div255(source_blue * global_alpha);
            }
            if (source_alpha == 0U)
                continue;
            if (source_alpha == ALPHA_OPAQUE)
                fb_store(destination,
                         s_ctx.bytes_per_pixel,
                         ARGB_OPAQUE_ALPHA | (source_red << 16) | (source_green << 8) | source_blue);
            else
                fb_store(destination,
                         s_ctx.bytes_per_pixel,
                         over_premul(fb_load(destination, s_ctx.bytes_per_pixel),
                                     source_alpha,
                                     source_red,
                                     source_green,
                                     source_blue));
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private (backend callbacks)
 ---------------------------------------------------------------------------------------------------------------------*/

static void fill_rect_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFU;
    if (alpha == 0U || !clip_rect(&x, &y, &width, &height, NULL, NULL))
        return;

    mark_dirty(x, y, width, height);
    wait_pending(); /* single transfer context; also fences before any CPU access below */

    if (alpha == ALPHA_OPAQUE && (width * height) >= s_ctx.config.min_dma_pixels)
    {
        cache_prepare_dst(x, y, width, height);
        s_ctx.regs->CR = DMA2D_CR_MODE_R2M;
        s_ctx.regs->OPFCCR = (uint32_t)s_ctx.config.format;
        s_ctx.regs->OCOLR = pack_ocolr(argb);
        s_ctx.regs->OMAR = (uint32_t)(uintptr_t)fb_at(x, y);
        s_ctx.regs->OOR = (uint32_t)(s_ctx.stride_pixels - width);
        s_ctx.regs->NLR = ((uint32_t)width << DMA2D_NLR_PL_SHIFT) | (uint32_t)height;
        launch(DMA2D_CR_MODE_R2M);
        return;
    }

    if (alpha == ALPHA_OPAQUE)
        cpu_fill_opaque(argb, x, y, width, height);
    else
        cpu_fill_blend(argb, alpha, x, y, width, height);
}

static void copy_rect_cb(const void* source, int source_stride_bytes, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    int source_x = 0, source_y = 0;
    if (!source || !clip_rect(&x, &y, &width, &height, &source_x, &source_y))
        return;

    mark_dirty(x, y, width, height);
    wait_pending();

    const uint8_t* source_bytes = (const uint8_t*)source;
    const uint8_t* dma_first =
        source_bytes + (size_t)source_y * (size_t)source_stride_bytes + (size_t)source_x * ARGB8888_BYTES;

    /* An opaque source is a pure PFC copy the peripheral can do outright; scanning the alpha
       channel first is one read pass, far cheaper than the read-modify-write blend it avoids.
       The alignment gate matches copy_rect_fmt_cb's: a word-misaligned ARGB8888 FGMAR raises a
       DMA2D configuration error (transfer refused, image silently absent), so such a source must
       take the CPU path below instead. */
    if ((width * height) >= s_ctx.config.min_dma_pixels && (((uintptr_t)dma_first & (ARGB8888_BYTES - 1U)) == 0U)
        && ((source_stride_bytes % ARGB8888_BYTES) == 0)
        && region_is_opaque(source_bytes, source_stride_bytes, source_x, source_y, width, height))
    {
        const uint8_t* first = dma_first;
        cache_clean_rows(first, (size_t)(height - 1) * (size_t)source_stride_bytes + (size_t)width * ARGB8888_BYTES);
        cache_prepare_dst(x, y, width, height);
        s_ctx.regs->CR = DMA2D_CR_MODE_M2M_PFC;
        s_ctx.regs->FGMAR = (uint32_t)(uintptr_t)first;
        s_ctx.regs->FGOR = (uint32_t)(source_stride_bytes / ARGB8888_BYTES - width);
        s_ctx.regs->FGPFCCR = DMA2D_CM_ARGB8888;
        s_ctx.regs->OPFCCR = (uint32_t)s_ctx.config.format;
        s_ctx.regs->OMAR = (uint32_t)(uintptr_t)fb_at(x, y);
        s_ctx.regs->OOR = (uint32_t)(s_ctx.stride_pixels - width);
        s_ctx.regs->NLR = ((uint32_t)width << DMA2D_NLR_PL_SHIFT) | (uint32_t)height;
        launch(DMA2D_CR_MODE_M2M_PFC);
        return;
    }

    /* Mixed-alpha source: DMA2D's blender expects straight-alpha foregrounds, but the engine
       hands premultiplied pixels — hardware blending would double-multiply. Composite on CPU. */
    cpu_copy_blend(source_bytes, source_stride_bytes, ALPHA_OPAQUE, source_x, source_y, x, y, width, height);
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
static void copy_rect_fmt_cb(
    const void* source, int source_stride_bytes, ERImageFormat format, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    int source_x = 0, source_y = 0;
    if (!source || !clip_rect(&x, &y, &width, &height, &source_x, &source_y))
        return;

    mark_dirty(x, y, width, height);
    wait_pending();

    const int source_bytes_per_pixel = (format == ER_IMG_RGB565) ? RGB565_BYTES : ARGB8888_BYTES;
    const uint8_t* first = (const uint8_t*)source + (size_t)source_y * (size_t)source_stride_bytes
                           + (size_t)source_x * (size_t)source_bytes_per_pixel;

    /* The DMA2D REQUIRES the source address aligned to the color mode's pixel size: an FGMAR of
       4n+2 with CM=ARGB8888 raises a CONFIGURATION ERROR - the transfer never starts, ISR.CEIF
       is set, and the image is silently absent. Both formats are therefore gated here, and a
       misaligned source takes the CPU replace below, which is correct at any alignment. The asset
       packer 4-aligns every image's pixels, so this rarely trips, but the backend must stay
       correct against any caller. */
    const bool aligned = ((((uintptr_t)first) & (uintptr_t)(source_bytes_per_pixel - 1)) == 0U)
                         && ((source_stride_bytes % source_bytes_per_pixel) == 0);

    if (aligned && (width * height) >= s_ctx.config.min_dma_pixels)
    {
        cache_clean_rows(
            first, (size_t)(height - 1) * (size_t)source_stride_bytes + (size_t)width * (size_t)source_bytes_per_pixel);
        cache_prepare_dst(x, y, width, height);
        s_ctx.regs->CR = DMA2D_CR_MODE_M2M_PFC;
        s_ctx.regs->FGMAR = (uint32_t)(uintptr_t)first;
        /* FGOR is the gap in PIXELS of the SOURCE format, not the destination's. */
        s_ctx.regs->FGOR = (uint32_t)(source_stride_bytes / source_bytes_per_pixel - width);
        s_ctx.regs->FGPFCCR = (format == ER_IMG_RGB565) ? DMA2D_CM_RGB565 : DMA2D_CM_ARGB8888;
        s_ctx.regs->OPFCCR = (uint32_t)s_ctx.config.format;
        s_ctx.regs->OMAR = (uint32_t)(uintptr_t)fb_at(x, y);
        s_ctx.regs->OOR = (uint32_t)(s_ctx.stride_pixels - width);
        s_ctx.regs->NLR = ((uint32_t)width << DMA2D_NLR_PL_SHIFT) | (uint32_t)height;
        launch(DMA2D_CR_MODE_M2M_PFC);
        return;
    }

    /* Below the floor where programming the peripheral pays for itself. Still a plain replace -
       the opacity guarantee means no destination read and no blend either way. */
    if (format == ER_IMG_RGB565)
    {
        for (int row = 0; row < height; row++)
        {
            const uint8_t* source_row = first + (size_t)row * (size_t)source_stride_bytes;
            uint8_t* destination = fb_at(x, y + row);
            for (int column = 0; column < width; column++, destination += s_ctx.bytes_per_pixel)
            {
                const uint32_t rgb565 = (uint32_t)source_row[column * RGB565_BYTES]
                                        | ((uint32_t)source_row[column * RGB565_BYTES + 1] << 8);
                const uint32_t red5 = (rgb565 >> 11) & 0x1FU;
                const uint32_t green6 = (rgb565 >> 5) & 0x3FU;
                const uint32_t blue5 = rgb565 & 0x1FU;
                /* Bit replication, matching the engine's own expansion exactly. */
                fb_store(destination,
                         s_ctx.bytes_per_pixel,
                         ARGB_OPAQUE_ALPHA | (((red5 << 3) | (red5 >> 2)) << 16)
                             | (((green6 << 2) | (green6 >> 4)) << 8) | ((blue5 << 3) | (blue5 >> 2)));
            }
        }
        return;
    }

    cpu_copy_blend((const uint8_t*)source, source_stride_bytes, ALPHA_OPAQUE, source_x, source_y, x, y, width, height);
}

static void blend_rect_cb(
    const void* source, int source_stride_bytes, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    if (alpha == ALPHA_OPAQUE)
    {
        copy_rect_cb(source, source_stride_bytes, x, y, width, height, ctx);
        return;
    }

    int source_x = 0, source_y = 0;
    if (!source || alpha == 0U || !clip_rect(&x, &y, &width, &height, &source_x, &source_y))
        return;

    mark_dirty(x, y, width, height);
    wait_pending();
    cpu_copy_blend((const uint8_t*)source, source_stride_bytes, alpha, source_x, source_y, x, y, width, height);
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
    s_ctx.config = *config;
    s_ctx.regs = (ErDma2dRegs*)config->dma2d;
    s_ctx.framebuffer = (uint8_t*)config->framebuffer;
    s_ctx.stride_pixels = config->stride_pixels > 0 ? config->stride_pixels : config->width;
    s_ctx.bytes_per_pixel = (config->format == ER_DMA2D_FB_RGB565)   ? RGB565_BYTES
                            : (config->format == ER_DMA2D_FB_RGB888) ? RGB888_BYTES
                                                                     : ARGB8888_BYTES;
    if (s_ctx.config.min_dma_pixels <= 0)
        s_ctx.config.min_dma_pixels = DMA2D_MIN_DMA_PIXELS_DEFAULT;

    s_ctx.regs->AMTCR = config->dead_time > 0 ? ((config->dead_time << DMA2D_AMTCR_DT_SHIFT) | DMA2D_AMTCR_EN) : 0U;

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
    s_ctx.framebuffer = (uint8_t*)framebuffer;
}

void* er_dma2d_backend_framebuffer(void)
{
    return s_ctx.framebuffer;
}

void er_dma2d_backend_wait(void)
{
    wait_pending();
}

bool er_dma2d_backend_take_dirty(int* x, int* y, int* width, int* height)
{
    if (s_ctx.dirty_x_end <= s_ctx.dirty_x0)
        return false;
    if (x)
        *x = s_ctx.dirty_x0;
    if (y)
        *y = s_ctx.dirty_y0;
    if (width)
        *width = s_ctx.dirty_x_end - s_ctx.dirty_x0;
    if (height)
        *height = s_ctx.dirty_y_end - s_ctx.dirty_y0;
    s_ctx.dirty_x0 = s_ctx.dirty_y0 = s_ctx.dirty_x_end = s_ctx.dirty_y_end = 0;
    return true;
}

void er_dma2d_backend_destroy(void)
{
    wait_pending();
    memset(&s_ctx, 0, sizeof(s_ctx));
}
