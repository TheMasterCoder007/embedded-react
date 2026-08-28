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

#ifndef ER_ESP32_LCD_PIE_BLEND_H
#define ER_ESP32_LCD_PIE_BLEND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Source-over blends premultiplied ARGB8888 pixels onto an RGB565 row using the
     *        ESP32-S3 PIE 128-bit SIMD unit, 8 pixels per iteration.
     *
     * Per pixel: a = src.A (scaled by ga when ga < 255); dst.C = src.C>>shift + dst.C*(256-a)>>8
     * in the 5/6-bit 565 domain, with per-pixel ordered-dither bias (or uniform round-to-nearest)
     * before each quantization — within one RGB565 LSB of the scalar reference
     * (over_premul_fast). Fully opaque and fully transparent source pixels are exact.
     *
     * @param[in,out] dst       RGB565 destination row. MUST be 16-byte aligned.
     * @param[in] src           Premultiplied ARGB8888 source row. MUST be 16-byte aligned.
     * @param[in] n8            Number of 8-pixel groups to process (pixels = n8 * 8).
     * @param[in] ga            Global alpha 0-255 applied to the source (255 = none).
     * @param[in] dither_phase  (x + y) parity of the row's first pixel (0/1) to keep the 2x2
     *                          ordered-dither checkerboard spatially stable; any other value
     *                          disables dithering (uniform round-to-nearest).
     */
    void er_pie_blend_row_565(uint16_t* dst, const uint32_t* src, int n8, uint8_t ga, int dither_phase);

    /**
     * @brief Source-over fills an RGB565 row with one translucent premultiplied ARGB8888 color,
     *        8 pixels per iteration.
     *
     * @param[in,out] dst  RGB565 destination row. MUST be 16-byte aligned.
     * @param[in] sp       Premultiplied ARGB8888 fill color (alpha in [1,254] is the useful range;
     *                     0 and 255 are handled by the caller's fast paths).
     * @param[in] n8       Number of 8-pixel groups to process.
     */
    void er_pie_fill_row_565(uint16_t* dst, uint32_t sp, int n8);

    /**
     * @brief Compares the PIE routines against the scalar reference across random pixels and
     *        edge alphas. Returns true when every channel is within one 565 LSB (and exact for
     *        alpha 0/255). Called once at backend init; a failure disables the PIE paths.
     */
    bool er_pie_blend_selftest(void);

    /**
     * @brief Returns a description of the last self-test failure ("" when it passed).
     */
    const char* er_pie_diag(void);

    /**
     * @brief Blends ONE pixel exactly as a PIE lane does — the shared scalar form of the SIMD math.
     *
     * The vector routines consume 8 pixels per iteration, so every row whose width is not a
     * multiple of 8 leaves a `w & 7` tail for the caller. That tail must quantize the same way the
     * lanes do; finishing it with the plain truncating store instead rounds the last few columns
     * systematically darker, which reads as a vertical bar down the right edge of every
     * translucent region (issue #134). Both the tail mop-up in renderer_backend.c and the
     * self-test's reference call this one function, so the two paths cannot drift apart.
     *
     * @param[in] d      Destination RGB565 pixel.
     * @param[in] sp     Premultiplied ARGB8888 source pixel.
     * @param[in] ga     Global alpha 0-255 applied to the source (255 = none).
     * @param[in] phase  0/1 select the 2x2 ordered-dither checkerboard (the (x + y) parity of the
     *                   row's first pixel), 2 = uniform round-to-nearest (dither off), and any
     *                   other value (use -1) = the truncating er_pie_fill_row_565() math.
     * @param[in] col    Column index within the row, which selects the checkerboard lane.
     */
    static inline uint16_t er_pie_blend_px_565(uint16_t d, uint32_t sp, uint8_t ga, int phase, int col)
    {
        uint32_t a = sp >> 24;
        uint32_t r = (sp >> 16) & 0xFFU;
        uint32_t g = (sp >> 8) & 0xFFU;
        uint32_t b = sp & 0xFFU;
        if (ga < 255U)
        {
            a = (a * ga) >> 8;
            r = (r * ga) >> 8;
            g = (g * ga) >> 8;
            b = (b * ga) >> 8;
        }
        uint32_t bias = 0U;
        uint32_t b3 = 0U;
        uint32_t b2 = 0U;
        if (phase == 0 || phase == 1)
        {
            const int par = (phase + col) & 1;
            bias = par ? 192U : 64U;
            b3 = par ? 6U : 2U;
            b2 = par ? 3U : 1U;
        }
        else if (phase == 2)
        {
            bias = 128U;
        }
        const uint32_t inv = 256U - a; /* a==0 -> dst unchanged; a==255 -> dst removed (exact edges) */
        const uint32_t dr5 = (d >> 11) & 31U;
        const uint32_t dg6 = (d >> 5) & 63U;
        const uint32_t db5 = d & 31U;
        uint32_t or5 = ((r + b3) >> 3) + ((dr5 * inv + bias) >> 8);
        uint32_t og6 = ((g + b2) >> 2) + ((dg6 * inv + bias) >> 8);
        uint32_t ob5 = ((b + b3) >> 3) + ((db5 * inv + bias) >> 8);
        if (or5 > 31U)
            or5 = 31U;
        if (og6 > 63U)
            og6 = 63U;
        if (ob5 > 31U)
            ob5 = 31U;
        return (uint16_t)((or5 << 11) | (og6 << 5) | ob5);
    }

#ifdef __cplusplus
}
#endif

#endif /* ER_ESP32_LCD_PIE_BLEND_H */
