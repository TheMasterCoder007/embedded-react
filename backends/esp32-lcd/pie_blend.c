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
 * fails, the backend falls back to the C loops and er_pie_diag() describes the first mismatch.
 *
 * Alignment contract: dst and src rows are 16-byte aligned (the backend stages rows through
 * aligned internal buffers, so this holds for every call).
 */

#include "pie_blend.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include <stdio.h>
#include <string.h>

/* Broadcast constants for EE.VLDBC.16 (address-based broadcast load). */
static const uint16_t s_k256 = 256U;
static const uint16_t s_m1f = 0x001FU;
static const uint16_t s_m3f = 0x003FU;
static const uint16_t s_m7f = 0x007FU;

/**
 * @brief Per-lane bias tables for ordered dithering, loaded with LD.QR at fixed offsets.
 *
 * d  (offset 0):  bias added to the dst*inv product before >>8.
 * s3 (offset 16): bias added to the ga-scaled source channel before >>3 (red/blue).
 * s2 (offset 32): bias added before >>2 (green).
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
    uint16_t d[8];
    uint16_t s3[8];
    uint16_t s2[8];
} PieDitherBlock;

/* Last self-test failure description (empty = passed); printable after boot via er_pie_diag(). */
static char s_pie_diag[128] = "";

const char* er_pie_diag(void)
{
    return s_pie_diag;
}

__attribute__((aligned(16))) static const PieDitherBlock s_dither[3] = {
    /* phase 0: lane 0 is an even (x+y) pixel */
    {{64, 192, 64, 192, 64, 192, 64, 192}, {2, 6, 2, 6, 2, 6, 2, 6}, {1, 3, 1, 3, 1, 3, 1, 3}},
    /* phase 1: lane 0 is an odd (x+y) pixel */
    {{192, 64, 192, 64, 192, 64, 192, 64}, {6, 2, 6, 2, 6, 2, 6, 2}, {3, 1, 3, 1, 3, 1, 3, 1}},
    /* uniform: plain round-to-nearest, no source bias (ER_LCD_DITHER=0) */
    {{128, 128, 128, 128, 128, 128, 128, 128}, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}},
};

void er_pie_blend_row_565(uint16_t* dst, const uint32_t* src, int n8, uint8_t ga, int dither_phase)
{
    if (n8 <= 0)
    {
        return;
    }
    const uint16_t* dr = dst; /* destination read cursor (vld advances it) */
    uint16_t* dw = dst;       /* destination write cursor */
    const uint16_t ga16 = ga;
    const PieDitherBlock* db = &s_dither[(dither_phase == 0 || dither_phase == 1) ? dither_phase : 2];

    if (ga == 255U)
    {
        __asm__ volatile(
            "loopnez        %[n8], .Lblend_end%= \n"
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
            : [src] "+a"(src), [dr] "+a"(dr), [dw] "+a"(dw)
            : [n8] "a"(n8), [k256] "a"(&s_k256), [db] "a"(db), [m1f] "a"(&s_m1f), [m3f] "a"(&s_m3f),
              [m7f] "a"(&s_m7f)
            : "memory");
        return;
    }

    __asm__ volatile(
        "loopnez        %[n8], .Lblendga_end%= \n"
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
        : [src] "+a"(src), [dr] "+a"(dr), [dw] "+a"(dw)
        : [n8] "a"(n8), [k256] "a"(&s_k256), [db] "a"(db), [m1f] "a"(&s_m1f), [m3f] "a"(&s_m3f),
          [m7f] "a"(&s_m7f), [ga] "a"(&ga16)
        : "memory");
}

void er_pie_fill_row_565(uint16_t* dst, uint32_t sp, int n8)
{
    if (n8 <= 0)
    {
        return;
    }
    const uint16_t* dr = dst;
    uint16_t* dw = dst;
    /* Constant lanes, computed once: source 565 contributions and the inverse alpha. */
    const uint16_t srcR5 = (uint16_t)(((sp >> 16) & 0xFFU) >> 3);
    const uint16_t srcG6 = (uint16_t)(((sp >> 8) & 0xFFU) >> 2);
    const uint16_t srcB5 = (uint16_t)((sp & 0xFFU) >> 3);
    const uint16_t inv = (uint16_t)(256U - (sp >> 24)); /* 256-a: exact at both alpha edges */

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
        : [dr] "+a"(dr), [dw] "+a"(dw)
        : [n8] "a"(n8), [b5] "a"(&srcB5), [r5] "a"(&srcR5), [g6] "a"(&srcG6), [inv] "a"(&inv), [m1f] "a"(&s_m1f),
          [m3f] "a"(&s_m3f)
        : "memory");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Self-test
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Scalar reference identical in rounding to the PIE routines (565-domain blend).
 *
 * @param[in] phase  Dither phase for the blend rows: 0/1 select the checkerboard tables
 *                   applied per column parity, 2 = uniform round-to-nearest, and -1 = the
 *                   fill-row math (truncating, no bias).
 * @param[in] col    Column index within the row (selects the checkerboard lane).
 */
static uint16_t ref_blend_px(uint16_t d, uint32_t sp, uint8_t ga, int phase, int col)
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
    const uint32_t inv = 256U - a; /* a==0 → dst unchanged; a==255 → dst removed (exact edges) */
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

/** @brief xorshift PRNG so the test needs no libc rand. */
static uint32_t xr(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

bool er_pie_blend_selftest(void)
{
    __attribute__((aligned(16))) static uint32_t src[64];
    __attribute__((aligned(16))) static uint16_t dst_pie[64];
    __attribute__((aligned(16))) static uint16_t dst_ref[64];
    uint32_t seed = 0x12345678U;

    const uint8_t gas[] = {255U, 128U, 37U};
    for (int phase = 0; phase <= 2; phase++)
    {
    for (unsigned gi = 0; gi < sizeof(gas); gi++)
    {
        const uint8_t ga = gas[gi];
        for (int round = 0; round < 8; round++)
        {
            for (int i = 0; i < 64; i++)
            {
                /* Premultiplied source: channels never exceed alpha. Force edge alphas often. */
                uint32_t a = xr(&seed) & 0xFFU;
                if ((xr(&seed) & 7U) == 0U)
                    a = 0U;
                if ((xr(&seed) & 7U) == 1U)
                    a = 255U;
                const uint32_t r = xr(&seed) % (a + 1U);
                const uint32_t g = xr(&seed) % (a + 1U);
                const uint32_t b = xr(&seed) % (a + 1U);
                src[i] = (a << 24) | (r << 16) | (g << 8) | b;
                const uint16_t d = (uint16_t)xr(&seed);
                dst_pie[i] = d;
                dst_ref[i] = d;
            }
            er_pie_blend_row_565(dst_pie, src, 64 / 8, ga, phase);
            for (int i = 0; i < 64; i++)
            {
                const uint16_t before = dst_ref[i];
                dst_ref[i] = ref_blend_px(dst_ref[i], src[i], ga, phase, i);
                if (dst_pie[i] != dst_ref[i])
                {
                    snprintf(s_pie_diag, sizeof(s_pie_diag),
                             "blend ph=%d ga=%u i=%d src=%08x before=%04x pie=%04x ref=%04x", phase, (unsigned)ga,
                             i, (unsigned)src[i], (unsigned)before, (unsigned)dst_pie[i], (unsigned)dst_ref[i]);
                    return false;
                }
                /* Source-over semantics at the edges: a fully transparent source pixel must be
                 * a strict no-op (and via the reference, a==0 implies ref == before). */
                if ((src[i] >> 24) == 0U && dst_pie[i] != before)
                {
                    return false;
                }
            }
        }
    }
    }

    /* Fill routine vs the same reference with a constant source. */
    for (int round = 0; round < 8; round++)
    {
        const uint32_t a = 1U + (xr(&seed) % 253U);
        const uint32_t sp = (a << 24) | ((xr(&seed) % (a + 1U)) << 16) | ((xr(&seed) % (a + 1U)) << 8)
                            | (xr(&seed) % (a + 1U));
        for (int i = 0; i < 64; i++)
        {
            const uint16_t d = (uint16_t)xr(&seed);
            dst_pie[i] = d;
            dst_ref[i] = d;
        }
        er_pie_fill_row_565(dst_pie, sp, 64 / 8);
        for (int i = 0; i < 64; i++)
        {
            dst_ref[i] = ref_blend_px(dst_ref[i], sp, 255U, -1, i);
            if (dst_pie[i] != dst_ref[i])
            {
                snprintf(s_pie_diag, sizeof(s_pie_diag), "fill i=%d sp=%08x pie=%04x ref=%04x", i, (unsigned)sp,
                         (unsigned)dst_pie[i], (unsigned)dst_ref[i]);
                return false;
            }
        }
    }
    return true;
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */
