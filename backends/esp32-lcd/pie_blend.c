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

/*
 * ESP32-S3 PIE (128-bit SIMD) blend routines for the esp32-lcd backend.
 *
 * The hot loop of every composited frame is "premultiplied ARGB8888 source over RGB565
 * destination" — scalar Xtensa spends ~40-50 cycles/pixel on it (unpack, alpha scale, blend,
 * repack). The PIE unit processes 8 pixels per iteration:
 *
 *   - Two EE.VUNZIP.8 rounds deinterleave 8 ARGB pixels into byte planes [B|G] and [R|A];
 *     EE.VZIP.8 against a zeroed register widens each plane to 16-bit lanes.
 *   - The 565 destination is unpacked with SAR-driven 32-bit lane shifts (EE.VSR.32) plus a
 *     per-lane AND mask: cross-lane contamination from the 32-bit shift lands above bit 5/6
 *     and is masked off.
 *   - EE.VMUL.S16 applies the (>> SAR) blend products; every operand is provably positive
 *     and within s16 range (premultiplied invariant: src.C <= src.A).
 *   - Results repack with EE.VSL.32 + EE.ORQ and store with EE.VST.128.IP.
 *
 * Blend: dst.C = (src.C + s_bias)>>shift + (dst.C*(256-a) + d_bias)>>8 in the 5/6-bit domain,
 * where the biases come from per-lane tables (see s_dither): a 2x2 ordered-dither checkerboard
 * keyed on (x+y) parity, or uniform round-to-nearest when dithering is off. The 256-a inverse
 * makes both alpha edges exact — a==0 leaves the destination bit-identical and a==255 replaces
 * it exactly; mid alphas stay within one 565 LSB of over_premul_fast in renderer_backend.c.
 * er_pie_blend_selftest() verifies every lane against a scalar reference at init — if it ever
 * fails, the backend falls back to the C loops and er_pie_diagnostics() describes the first mismatch.
 *
 * Alignment contract: dst and src rows are 16-byte aligned (the backend stages rows through
 * aligned internal buffers, so this holds for every call).
 */

#include "pie_blend.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include <stdio.h>
#include <string.h>

/** @brief Bytes kept of the last self-test failure. */
#define DIAG_BYTES 128

/** @brief Self-test size: pixels per row, rows per case, and the PRNG seed. */
#define SELFTEST_PIXELS 64
#define SELFTEST_ROUNDS 8
#define SELFTEST_SEED 0x12345678U

/** @brief The fill test draws its alpha from [1, SELFTEST_FILL_ALPHAS], skipping the fast-path edges. */
#define SELFTEST_FILL_ALPHAS 253U

/* Broadcast constants for EE.VLDBC.16 (address-based broadcast load). */
static const uint16_t s_inverse_base = ER_PIE_INVERSE_BASE;
static const uint16_t s_mask_5bit = 0x001FU;
static const uint16_t s_mask_6bit = 0x003FU;
static const uint16_t s_mask_7bit = 0x007FU;

/**
 * @brief Per-lane bias tables for ordered dithering, loaded with LD.QR at fixed offsets.
 *
 * destination_bias (offset 0):  bias added to the dst*inv product before >>8.
 * bias_5bit        (offset 16): bias added to the global-alpha-scaled source channel before >>3 (red/blue).
 * bias_6bit        (offset 32): bias added before >>2 (green).
 *
 * Blocks 0/1 are the two phases of a 2x2 checkerboard keyed on (x+y) parity — the quantization
 * error becomes a fine, spatially stable texture instead of a frame-to-frame flip on animated
 * fades. Block 2 is the uniform round-to-nearest math (dither disabled). Bias bounds keep the
 * alpha edges exact: the dst bias never reaches 256 (a==0 stays a strict no-op, and 63+192<256
 * keeps a==255 an exact replacement); the source biases stay below their shift quantum, and the
 * final per-channel VMIN clamp absorbs the rounding overflow at half-alphas.
 */
typedef struct
{
    uint16_t destination_bias[ER_PIE_LANES];
    uint16_t bias_5bit[ER_PIE_LANES];
    uint16_t bias_6bit[ER_PIE_LANES];
} PieDitherBlock;

/* Last self-test failure description (empty = passed); printable after boot via er_pie_diagnostics(). */
static char s_pie_diag[DIAG_BYTES] = "";

const char* er_pie_diagnostics(void)
{
    return s_pie_diag;
}

/** @brief Eight lanes alternating between two values, starting with the first. */
#define ALTERNATE_LANES(first, second) {first, second, first, second, first, second, first, second}

__attribute__((aligned(16))) static const PieDitherBlock s_dither[ER_PIE_DITHER_UNIFORM + 1] = {
    /* phase 0: lane 0 is an even (x+y) pixel */
    {ALTERNATE_LANES(ER_PIE_BIAS_LOW, ER_PIE_BIAS_HIGH),
     ALTERNATE_LANES(ER_PIE_BIAS_5BIT_LOW, ER_PIE_BIAS_5BIT_HIGH),
     ALTERNATE_LANES(ER_PIE_BIAS_6BIT_LOW, ER_PIE_BIAS_6BIT_HIGH)},
    /* phase 1: lane 0 is an odd (x+y) pixel */
    {ALTERNATE_LANES(ER_PIE_BIAS_HIGH, ER_PIE_BIAS_LOW),
     ALTERNATE_LANES(ER_PIE_BIAS_5BIT_HIGH, ER_PIE_BIAS_5BIT_LOW),
     ALTERNATE_LANES(ER_PIE_BIAS_6BIT_HIGH, ER_PIE_BIAS_6BIT_LOW)},
    /* uniform: plain round-to-nearest, no source bias (ER_LCD_DITHER=0) */
    {ALTERNATE_LANES(ER_PIE_BIAS_ROUND, ER_PIE_BIAS_ROUND), ALTERNATE_LANES(0, 0), ALTERNATE_LANES(0, 0)},
};

void er_pie_blend_row_565(
    uint16_t* destination, const uint32_t* source, int group_count, uint8_t global_alpha, int dither_phase)
{
    if (group_count <= 0)
    {
        return;
    }
    const uint16_t* destination_read = destination; /* destination read cursor (vld advances it) */
    uint16_t* destination_write = destination;      /* destination write cursor */
    const uint16_t global_alpha16 = global_alpha;
    const PieDitherBlock* dither =
        &s_dither[(dither_phase == 0 || dither_phase == 1) ? dither_phase : ER_PIE_DITHER_UNIFORM];

    if (global_alpha == ER_PIE_ALPHA_OPAQUE)
    {
        __asm__ volatile("loopnez        %[n8], .Lblend_end%= \n"
                         /*  src pixels: q0 = px0-3, q1 = px4-7; dst: q2 = 8x565 */
                         "ee.vld.128.ip  q0, %[src], 16      \n"
                         "ee.vld.128.ip  q1, %[src], 16      \n"
                         "ee.vld.128.ip  q2, %[dr], 16       \n"
                         /* byte-plane deinterleave: q0=[B|G], q1=[R|A] */
                         "ee.vunzip.8    q0, q1              \n"
                         "ee.vunzip.8    q0, q1              \n"
                         /* widen to 16-bit lanes: q0=B, q3=G, q1=R, q4=A */
                         "ee.zero.q      q3                  \n"
                         "ee.vzip.8      q0, q3              \n"
                         "ee.zero.q      q4                  \n"
                         "ee.vzip.8      q1, q4              \n"
                         /* inv = 256 - a: a==0 leaves dst exactly unchanged (dst*256>>8), a==255 removes it
                          * exactly — true source-over at both edges. q7 then becomes the +128 rounding bias. */
                         "ee.vldbc.16    q7, %[k256]         \n"
                         "ee.vsubs.s16   q4, q7, q4          \n"
                         "ld.qr          q7, %[db], 0        \n" /* per-lane dst rounding/dither bias */
                         /* ---- red: out = min(31, srcR5 + (dstR5*inv + 128 >> 8)) ---- */
                         "ssai           11                  \n"
                         "ee.vsr.32      q5, q2              \n"
                         "ee.vldbc.16    q6, %[m1f]          \n"
                         "ee.andq        q5, q5, q6          \n" /* dstR5 */
                         "ssai           0                  \n"
                         "ee.vmul.s16    q5, q5, q4          \n" /* dstR5*inv (full product) */
                         "ee.vadds.s16   q5, q5, q7          \n" /* +128: round to nearest */
                         "ssai           8                  \n"
                         "ee.vsr.32      q5, q5              \n"
                         "ee.andq        q5, q5, q6          \n" /* >>8, clear cross-lane spill */
                         "ld.qr          q6, %[db], 16       \n" /* source dither bias (>>3 quantum) */
                         "ee.vadds.s16   q1, q1, q6          \n"
                         "ssai           3                  \n"
                         "ee.vsr.32      q1, q1              \n"
                         "ee.vldbc.16    q6, %[m3f]          \n"
                         "ee.andq        q1, q1, q6          \n" /* wide: keep the +bias overflow bit, drop spill */
                         "ee.vldbc.16    q6, %[m1f]          \n" /* srcR5 */
                         "ee.vadds.s16   q1, q1, q5          \n"
                         "ee.vmin.s16    q1, q1, q6          \n" /* clamp: rounding can reach 32 at half-alphas */
                         /* ---- blue ---- */
                         "ee.andq        q5, q2, q6          \n" /* dstB5 */
                         "ssai           0                  \n"
                         "ee.vmul.s16    q5, q5, q4          \n"
                         "ee.vadds.s16   q5, q5, q7          \n"
                         "ssai           8                  \n"
                         "ee.vsr.32      q5, q5              \n"
                         "ee.andq        q5, q5, q6          \n"
                         "ld.qr          q6, %[db], 16       \n"
                         "ee.vadds.s16   q0, q0, q6          \n"
                         "ssai           3                  \n"
                         "ee.vsr.32      q0, q0              \n"
                         "ee.vldbc.16    q6, %[m3f]          \n"
                         "ee.andq        q0, q0, q6          \n" /* wide: keep the +bias overflow bit, drop spill */
                         "ee.vldbc.16    q6, %[m1f]          \n" /* srcB5 */
                         "ee.vadds.s16   q0, q0, q5          \n"
                         "ee.vmin.s16    q0, q0, q6          \n"
                         /* ---- green (6-bit mask/clamp) ---- */
                         "ssai           5                  \n"
                         "ee.vsr.32      q5, q2              \n"
                         "ee.vldbc.16    q6, %[m3f]          \n"
                         "ee.andq        q5, q5, q6          \n" /* dstG6 */
                         "ssai           0                  \n"
                         "ee.vmul.s16    q5, q5, q4          \n"
                         "ee.vadds.s16   q5, q5, q7          \n"
                         "ssai           8                  \n"
                         "ee.vsr.32      q5, q5              \n"
                         "ee.andq        q5, q5, q6          \n"
                         "ld.qr          q6, %[db], 32       \n" /* source dither bias (>>2 quantum) */
                         "ee.vadds.s16   q3, q3, q6          \n"
                         "ssai           2                  \n"
                         "ee.vsr.32      q3, q3              \n"
                         "ee.vldbc.16    q6, %[m7f]          \n"
                         "ee.andq        q3, q3, q6          \n" /* wide: keep the +bias overflow bit, drop spill */
                         "ee.vldbc.16    q6, %[m3f]          \n" /* srcG6 */
                         "ee.vadds.s16   q3, q3, q5          \n"
                         "ee.vmin.s16    q3, q3, q6          \n"
                         /* ---- repack (R<<11 | G<<5 | B) and store ---- */
                         "ssai           11                  \n"
                         "ee.vsl.32      q1, q1              \n"
                         "ssai           5                  \n"
                         "ee.vsl.32      q3, q3              \n"
                         "ee.orq         q1, q1, q3          \n"
                         "ee.orq         q1, q1, q0          \n"
                         "ee.vst.128.ip  q1, %[dw], 16       \n"
                         ".Lblend_end%=:                     \n"
                         : [src] "+a"(source), [dr] "+a"(destination_read), [dw] "+a"(destination_write)
                         : [n8] "a"(group_count),
                           [k256] "a"(&s_inverse_base),
                           [db] "a"(dither),
                           [m1f] "a"(&s_mask_5bit),
                           [m3f] "a"(&s_mask_6bit),
                           [m7f] "a"(&s_mask_7bit)
                         : "memory");
        return;
    }

    __asm__ volatile("loopnez        %[n8], .Lblendga_end%= \n"
                     "ee.vld.128.ip  q0, %[src], 16      \n"
                     "ee.vld.128.ip  q1, %[src], 16      \n"
                     "ee.vld.128.ip  q2, %[dr], 16       \n"
                     "ee.vunzip.8    q0, q1              \n"
                     "ee.vunzip.8    q0, q1              \n"
                     "ee.zero.q      q3                  \n"
                     "ee.vzip.8      q0, q3              \n"
                     "ee.zero.q      q4                  \n"
                     "ee.vzip.8      q1, q4              \n"
                     /* scale source channels AND alpha by the global alpha: c = c*ga >> 8 */
                     "ee.vldbc.16    q6, %[ga]           \n"
                     "ssai           8                  \n"
                     "ee.vmul.s16    q0, q0, q6          \n"
                     "ee.vmul.s16    q1, q1, q6          \n"
                     "ee.vmul.s16    q3, q3, q6          \n"
                     "ee.vmul.s16    q4, q4, q6          \n"
                     /* inv = 256 - a_scaled (exact at both alpha edges); q7 then holds the rounding bias */
                     "ee.vldbc.16    q7, %[k256]         \n"
                     "ee.vsubs.s16   q4, q7, q4          \n"
                     "ld.qr          q7, %[db], 0        \n" /* per-lane dst rounding/dither bias */
                     /* ---- red ---- */
                     "ssai           11                  \n"
                     "ee.vsr.32      q5, q2              \n"
                     "ee.vldbc.16    q6, %[m1f]          \n"
                     "ee.andq        q5, q5, q6          \n"
                     "ssai           0                  \n"
                     "ee.vmul.s16    q5, q5, q4          \n"
                     "ee.vadds.s16   q5, q5, q7          \n"
                     "ssai           8                  \n"
                     "ee.vsr.32      q5, q5              \n"
                     "ee.andq        q5, q5, q6          \n"
                     "ld.qr          q6, %[db], 16       \n"
                     "ee.vadds.s16   q1, q1, q6          \n"
                     "ssai           3                  \n"
                     "ee.vsr.32      q1, q1              \n"
                     "ee.vldbc.16    q6, %[m3f]          \n"
                     "ee.andq        q1, q1, q6          \n" /* wide: keep the +bias overflow bit, drop spill */
                     "ee.vldbc.16    q6, %[m1f]          \n"
                     "ee.vadds.s16   q1, q1, q5          \n"
                     "ee.vmin.s16    q1, q1, q6          \n"
                     /* ---- blue ---- */
                     "ee.andq        q5, q2, q6          \n"
                     "ssai           0                  \n"
                     "ee.vmul.s16    q5, q5, q4          \n"
                     "ee.vadds.s16   q5, q5, q7          \n"
                     "ssai           8                  \n"
                     "ee.vsr.32      q5, q5              \n"
                     "ee.andq        q5, q5, q6          \n"
                     "ld.qr          q6, %[db], 16       \n"
                     "ee.vadds.s16   q0, q0, q6          \n"
                     "ssai           3                  \n"
                     "ee.vsr.32      q0, q0              \n"
                     "ee.vldbc.16    q6, %[m3f]          \n"
                     "ee.andq        q0, q0, q6          \n" /* wide: keep the +bias overflow bit, drop spill */
                     "ee.vldbc.16    q6, %[m1f]          \n"
                     "ee.vadds.s16   q0, q0, q5          \n"
                     "ee.vmin.s16    q0, q0, q6          \n"
                     /* ---- green ---- */
                     "ssai           5                  \n"
                     "ee.vsr.32      q5, q2              \n"
                     "ee.vldbc.16    q6, %[m3f]          \n"
                     "ee.andq        q5, q5, q6          \n"
                     "ssai           0                  \n"
                     "ee.vmul.s16    q5, q5, q4          \n"
                     "ee.vadds.s16   q5, q5, q7          \n"
                     "ssai           8                  \n"
                     "ee.vsr.32      q5, q5              \n"
                     "ee.andq        q5, q5, q6          \n"
                     "ld.qr          q6, %[db], 32       \n"
                     "ee.vadds.s16   q3, q3, q6          \n"
                     "ssai           2                  \n"
                     "ee.vsr.32      q3, q3              \n"
                     "ee.vldbc.16    q6, %[m7f]          \n"
                     "ee.andq        q3, q3, q6          \n" /* wide: keep the +bias overflow bit, drop spill */
                     "ee.vldbc.16    q6, %[m3f]          \n"
                     "ee.vadds.s16   q3, q3, q5          \n"
                     "ee.vmin.s16    q3, q3, q6          \n"
                     /* ---- repack + store ---- */
                     "ssai           11                  \n"
                     "ee.vsl.32      q1, q1              \n"
                     "ssai           5                  \n"
                     "ee.vsl.32      q3, q3              \n"
                     "ee.orq         q1, q1, q3          \n"
                     "ee.orq         q1, q1, q0          \n"
                     "ee.vst.128.ip  q1, %[dw], 16       \n"
                     ".Lblendga_end%=:                   \n"
                     : [src] "+a"(source), [dr] "+a"(destination_read), [dw] "+a"(destination_write)
                     : [n8] "a"(group_count),
                       [k256] "a"(&s_inverse_base),
                       [db] "a"(dither),
                       [m1f] "a"(&s_mask_5bit),
                       [m3f] "a"(&s_mask_6bit),
                       [m7f] "a"(&s_mask_7bit),
                       [ga] "a"(&global_alpha16)
                     : "memory");
}

void er_pie_fill_row_565(uint16_t* destination, uint32_t source_pixel, int group_count)
{
    if (group_count <= 0)
    {
        return;
    }
    const uint16_t* destination_read = destination;
    uint16_t* destination_write = destination;
    /* Constant lanes, computed once: source 565 contributions and the inverse alpha. */
    const uint16_t source_red5 = (uint16_t)(((source_pixel >> 16) & 0xFFU) >> 3);
    const uint16_t source_green6 = (uint16_t)(((source_pixel >> 8) & 0xFFU) >> 2);
    const uint16_t source_blue5 = (uint16_t)((source_pixel & 0xFFU) >> 3);
    const uint16_t inverse_alpha =
        (uint16_t)(ER_PIE_INVERSE_BASE - (source_pixel >> 24)); /* 256-a: exact at both alpha edges */

    __asm__ volatile(
        /* persistent constants: q0=srcB5, q1=srcR5, q3=srcG6, q4=inv */
        "ee.vldbc.16    q0, %[b5]           \n"
        "ee.vldbc.16    q1, %[r5]           \n"
        "ee.vldbc.16    q3, %[g6]           \n"
        "ee.vldbc.16    q4, %[inv]          \n"
        "loopnez        %[n8], .Lfill_end%= \n"
        "ee.vld.128.ip  q2, %[dr], 16       \n"
        /* ---- red: q5 = (srcR5 + dstR5*inv>>8) << 11 ---- */
        "ssai           11                  \n"
        "ee.vsr.32      q5, q2              \n"
        "ee.vldbc.16    q6, %[m1f]          \n"
        "ee.andq        q5, q5, q6          \n"
        "ssai           8                  \n"
        "ee.vmul.s16    q5, q5, q4          \n"
        "ee.vadds.s16   q5, q5, q1          \n"
        "ssai           11                  \n"
        "ee.vsl.32      q5, q5              \n"
        /* ---- blue: q7 = srcB5 + dstB5*inv>>8 ---- */
        "ee.andq        q7, q2, q6          \n"
        "ssai           8                  \n"
        "ee.vmul.s16    q7, q7, q4          \n"
        "ee.vadds.s16   q7, q7, q0          \n"
        "ee.orq         q5, q5, q7          \n"
        /* ---- green: q7 = (srcG6 + dstG6*inv>>8) << 5 ---- */
        "ssai           5                  \n"
        "ee.vsr.32      q7, q2              \n"
        "ee.vldbc.16    q6, %[m3f]          \n"
        "ee.andq        q7, q7, q6          \n"
        "ssai           8                  \n"
        "ee.vmul.s16    q7, q7, q4          \n"
        "ee.vadds.s16   q7, q7, q3          \n"
        "ssai           5                  \n"
        "ee.vsl.32      q7, q7              \n"
        "ee.orq         q5, q5, q7          \n"
        "ee.vst.128.ip  q5, %[dw], 16       \n"
        ".Lfill_end%=:                      \n"
        : [dr] "+a"(destination_read), [dw] "+a"(destination_write)
        : [n8] "a"(group_count),
          [b5] "a"(&source_blue5),
          [r5] "a"(&source_red5),
          [g6] "a"(&source_green6),
          [inv] "a"(&inverse_alpha),
          [m1f] "a"(&s_mask_5bit),
          [m3f] "a"(&s_mask_6bit)
        : "memory");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Self-test
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief xorshift PRNG so the test needs no libc rand. */
static uint32_t xr(uint32_t* state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

bool er_pie_blend_selftest(void)
{
    __attribute__((aligned(16))) static uint32_t source[SELFTEST_PIXELS];
    __attribute__((aligned(16))) static uint16_t destination_pie[SELFTEST_PIXELS];
    __attribute__((aligned(16))) static uint16_t destination_reference[SELFTEST_PIXELS];
    s_pie_diag[0] = '\0';
    uint32_t seed = SELFTEST_SEED;

    const uint8_t global_alphas[] = {255U, 128U, 37U};
#if defined(ER_LCD_DITHER) && (ER_LCD_DITHER == 0)
    for (int phase = ER_PIE_DITHER_UNIFORM; phase <= ER_PIE_DITHER_UNIFORM; phase++)
#else
    for (int phase = 0; phase <= ER_PIE_DITHER_UNIFORM; phase++)
#endif
    {
        for (unsigned alpha_index = 0; alpha_index < sizeof(global_alphas); alpha_index++)
        {
            const uint8_t global_alpha = global_alphas[alpha_index];
            for (int round = 0; round < SELFTEST_ROUNDS; round++)
            {
                for (int pixel = 0; pixel < SELFTEST_PIXELS; pixel++)
                {
                    /* Premultiplied source: channels never exceed alpha. Force edge alphas often. */
                    uint32_t alpha = xr(&seed) & 0xFFU;
                    if ((xr(&seed) & 7U) == 0U)
                        alpha = 0U;
                    if ((xr(&seed) & 7U) == 1U)
                        alpha = ER_PIE_ALPHA_OPAQUE;
                    const uint32_t red = xr(&seed) % (alpha + 1U);
                    const uint32_t green = xr(&seed) % (alpha + 1U);
                    const uint32_t blue = xr(&seed) % (alpha + 1U);
                    source[pixel] = (alpha << 24) | (red << 16) | (green << 8) | blue;
                    const uint16_t destination_pixel = (uint16_t)xr(&seed);
                    destination_pie[pixel] = destination_pixel;
                    destination_reference[pixel] = destination_pixel;
                }
                er_pie_blend_row_565(destination_pie, source, SELFTEST_PIXELS / ER_PIE_LANES, global_alpha, phase);
                for (int pixel = 0; pixel < SELFTEST_PIXELS; pixel++)
                {
                    const uint16_t before = destination_reference[pixel];
                    destination_reference[pixel] =
                        er_pie_blend_pixel_565(destination_reference[pixel], source[pixel], global_alpha, phase, pixel);
                    if (destination_pie[pixel] != destination_reference[pixel])
                    {
                        snprintf(s_pie_diag,
                                 sizeof(s_pie_diag),
                                 "blend ph=%d ga=%u i=%d src=%08x before=%04x pie=%04x ref=%04x",
                                 phase,
                                 (unsigned)global_alpha,
                                 pixel,
                                 (unsigned)source[pixel],
                                 (unsigned)before,
                                 (unsigned)destination_pie[pixel],
                                 (unsigned)destination_reference[pixel]);
                        return false;
                    }
                    /* Source-over semantics at the edges: a fully transparent source pixel must be
                     * a strict no-op (and via the reference, a==0 implies ref == before). */
                    if ((source[pixel] >> 24) == 0U && destination_pie[pixel] != before)
                    {
                        return false;
                    }
                }
            }
        }
    }

    /* Fill routine vs the same reference with a constant source. */
    for (int round = 0; round < SELFTEST_ROUNDS; round++)
    {
        const uint32_t alpha = 1U + (xr(&seed) % SELFTEST_FILL_ALPHAS);
        const uint32_t source_pixel = (alpha << 24) | ((xr(&seed) % (alpha + 1U)) << 16)
                                      | ((xr(&seed) % (alpha + 1U)) << 8) | (xr(&seed) % (alpha + 1U));
        for (int pixel = 0; pixel < SELFTEST_PIXELS; pixel++)
        {
            const uint16_t destination_pixel = (uint16_t)xr(&seed);
            destination_pie[pixel] = destination_pixel;
            destination_reference[pixel] = destination_pixel;
        }
        er_pie_fill_row_565(destination_pie, source_pixel, SELFTEST_PIXELS / ER_PIE_LANES);
        for (int pixel = 0; pixel < SELFTEST_PIXELS; pixel++)
        {
            destination_reference[pixel] = er_pie_blend_pixel_565(
                destination_reference[pixel], source_pixel, ER_PIE_ALPHA_OPAQUE, ER_PIE_FILL_MATH, pixel);
            if (destination_pie[pixel] != destination_reference[pixel])
            {
                snprintf(s_pie_diag,
                         sizeof(s_pie_diag),
                         "fill i=%d sp=%08x pie=%04x ref=%04x",
                         pixel,
                         (unsigned)source_pixel,
                         (unsigned)destination_pie[pixel],
                         (unsigned)destination_reference[pixel]);
                return false;
            }
        }
    }
    return true;
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */
