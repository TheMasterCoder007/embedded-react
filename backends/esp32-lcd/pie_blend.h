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

/** @brief Pixels per PIE iteration. */
#define ER_PIE_LANES 8

/** @brief A global alpha of this value leaves the source unscaled. */
#define ER_PIE_ALPHA_OPAQUE 255U

/** @brief The inverse weight is this minus alpha, which keeps both alpha edges exact. */
#define ER_PIE_INVERSE_BASE 256U

/** @brief Destination rounding biases: the two checkerboard lanes, and plain round-to-nearest. */
#define ER_PIE_BIAS_LOW 64U
#define ER_PIE_BIAS_HIGH 192U
#define ER_PIE_BIAS_ROUND 128U

/** @brief Source dither biases for the 5-bit (>> 3) and 6-bit (>> 2) channels. */
#define ER_PIE_BIAS_5BIT_LOW 2U
#define ER_PIE_BIAS_5BIT_HIGH 6U
#define ER_PIE_BIAS_6BIT_LOW 1U
#define ER_PIE_BIAS_6BIT_HIGH 3U

/** @brief The dither phase that rounds uniformly; 0 and 1 pick a checkerboard phase. */
#define ER_PIE_DITHER_UNIFORM 2

/** @brief The phase that selects the truncating er_pie_fill_row_565() math. */
#define ER_PIE_FILL_MATH (-1)

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Source-over blends premultiplied ARGB8888 pixels onto an RGB565 row using the
     *        ESP32-S3 PIE 128-bit SIMD unit, 8 pixels per iteration.
     *
     * Per pixel: a = src.A (scaled by global_alpha when it is below 255); dst.C = src.C>>shift + dst.C*(256-a)>>8
     * in the 5/6-bit 565 domain, with per-pixel ordered-dither bias (or uniform round-to-nearest)
     * before each quantization — within one RGB565 LSB of the scalar reference
     * (over_premul_fast). Fully opaque and fully transparent source pixels are exact.
     *
     * @param[in,out] destination       RGB565 destination row. MUST be 16-byte aligned.
     * @param[in] source           Premultiplied ARGB8888 source row. MUST be 16-byte aligned.
     * @param[in] group_count            Number of 8-pixel groups to process (pixels = group_count * 8).
     * @param[in] global_alpha            Global alpha 0-255 applied to the source (255 = none).
     * @param[in] dither_phase  (x + y) parity of the row's first pixel (0/1) to keep the 2x2
     *                          ordered-dither checkerboard spatially stable; any other value
     *                          disables dithering (uniform round-to-nearest).
     */
    void er_pie_blend_row_565(
        uint16_t* destination, const uint32_t* source, int group_count, uint8_t global_alpha, int dither_phase);

    /**
     * @brief Source-over fills an RGB565 row with one translucent premultiplied ARGB8888 color,
     *        8 pixels per iteration.
     *
     * @param[in,out] destination  RGB565 destination row. MUST be 16-byte aligned.
     * @param[in] source_pixel       Premultiplied ARGB8888 fill color (alpha in [1,254] is the useful range;
     *                     0 and 255 are handled by the caller's fast paths).
     * @param[in] group_count       Number of 8-pixel groups to process.
     */
    void er_pie_fill_row_565(uint16_t* destination, uint32_t source_pixel, int group_count);

    /**
     * @brief Compares the PIE routines against the scalar reference across random pixels and
     *        edge alphas. Returns true when every channel is within one 565 LSB (and exact for
     *        alpha 0/255). Called once at backend init; a failure disables the PIE paths.
     */
    bool er_pie_blend_selftest(void);

    /**
     * @brief Returns a description of the last self-test failure ("" when it passed).
     */
    const char* er_pie_diagnostics(void);

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
     * @param[in] destination_pixel      Destination RGB565 pixel.
     * @param[in] source_pixel     Premultiplied ARGB8888 source pixel.
     * @param[in] global_alpha     Global alpha 0-255 applied to the source (255 = none).
     * @param[in] phase  0/1 select the 2x2 ordered-dither checkerboard (the (x + y) parity of the
     *                   row's first pixel), 2 = uniform round-to-nearest (dither off), and any
     *                   other value (use -1) = the truncating er_pie_fill_row_565() math.
     * @param[in] column    Column index within the row, which selects the checkerboard lane.
     */
    static inline uint16_t er_pie_blend_pixel_565(
        uint16_t destination_pixel, uint32_t source_pixel, uint8_t global_alpha, int phase, int column)
    {
        uint32_t alpha = source_pixel >> 24;
        uint32_t red = (source_pixel >> 16) & 0xFFU;
        uint32_t green = (source_pixel >> 8) & 0xFFU;
        uint32_t blue = source_pixel & 0xFFU;
        if (global_alpha < ER_PIE_ALPHA_OPAQUE)
        {
            alpha = (alpha * global_alpha) >> 8;
            red = (red * global_alpha) >> 8;
            green = (green * global_alpha) >> 8;
            blue = (blue * global_alpha) >> 8;
        }
        uint32_t bias = 0U;
        uint32_t bias_5bit = 0U;
        uint32_t bias_6bit = 0U;
        if (phase == 0 || phase == 1)
        {
            const int parity = (phase + column) & 1;
            bias = parity ? ER_PIE_BIAS_HIGH : ER_PIE_BIAS_LOW;
            bias_5bit = parity ? ER_PIE_BIAS_5BIT_HIGH : ER_PIE_BIAS_5BIT_LOW;
            bias_6bit = parity ? ER_PIE_BIAS_6BIT_HIGH : ER_PIE_BIAS_6BIT_LOW;
        }
        else if (phase == ER_PIE_DITHER_UNIFORM)
        {
            bias = ER_PIE_BIAS_ROUND;
        }
        const uint32_t inverse_alpha =
            ER_PIE_INVERSE_BASE - alpha; /* a==0 -> dst unchanged; a==255 -> dst removed (exact edges) */
        const uint32_t destination_red5 = (destination_pixel >> 11) & 31U;
        const uint32_t destination_green6 = (destination_pixel >> 5) & 63U;
        const uint32_t destination_blue5 = destination_pixel & 31U;
        uint32_t out_red5 = ((red + bias_5bit) >> 3) + ((destination_red5 * inverse_alpha + bias) >> 8);
        uint32_t out_green6 = ((green + bias_6bit) >> 2) + ((destination_green6 * inverse_alpha + bias) >> 8);
        uint32_t out_blue5 = ((blue + bias_5bit) >> 3) + ((destination_blue5 * inverse_alpha + bias) >> 8);
        if (out_red5 > 31U)
            out_red5 = 31U;
        if (out_green6 > 63U)
            out_green6 = 63U;
        if (out_blue5 > 31U)
            out_blue5 = 31U;
        return (uint16_t)((out_red5 << 11) | (out_green6 << 5) | out_blue5);
    }

#ifdef __cplusplus
}
#endif

#endif /* ER_ESP32_LCD_PIE_BLEND_H */
