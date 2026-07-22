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
     * Per pixel: a = src.A (scaled by ga when ga < 255); dst.C = src.C>>shift + dst.C*(255-a)>>8
     * in the 5/6-bit 565 domain — within one 565 LSB of the scalar reference (over_premul_fast).
     * Fully opaque and fully transparent source pixels produce exact results.
     *
     * @param[in,out] dst  RGB565 destination row. MUST be 16-byte aligned.
     * @param[in] src      Premultiplied ARGB8888 source row. MUST be 16-byte aligned.
     * @param[in] n8       Number of 8-pixel groups to process (pixels = n8 * 8).
     * @param[in] ga       Global alpha 0-255 applied to the source (255 = none).
     */
    void er_pie_blend_row_565(uint16_t* dst, const uint32_t* src, int n8, uint8_t ga);

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

#ifdef __cplusplus
}
#endif

#endif /* ER_ESP32_LCD_PIE_BLEND_H */
