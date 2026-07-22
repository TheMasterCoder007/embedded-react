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
 * Rounding matches over_premul_fast in renderer_backend.c to within one 565 LSB (the source
 * contribution truncates to 5/6 bits before the add instead of after); fully opaque and fully
 * transparent pixels are exact. er_pie_blend_selftest() verifies this against a scalar
 * reference at init — if it ever fails, the backend falls back to the C loops.
 *
 * Alignment contract: dst and src rows are 16-byte aligned (the backend stages rows through
 * aligned internal buffers, so this holds for every call).
 */

#include "pie_blend.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include <string.h>

/* Broadcast constants for EE.VLDBC.16 (address-based broadcast load). */
static const uint16_t s_k255 = 255U;
static const uint16_t s_m1f = 0x001FU;
static const uint16_t s_m3f = 0x003FU;

void er_pie_blend_row_565(uint16_t* dst, const uint32_t* src, int n8, uint8_t ga)
{
    if (n8 <= 0)
    {
        return;
    }
    const uint16_t* dr = dst; /* destination read cursor (vld advances it) */
    uint16_t* dw = dst;       /* destination write cursor */
    const uint16_t ga16 = ga;

    if (ga == 255U)
    {
        __asm__ volatile(
            /* q7 = 255 broadcast (persistent across the loop) */
            "ee.vldbc.16    q7, %[k255]         \n"
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
            /* inv = 255 - a  (a <= 255, so XOR with 255 is exact subtraction) */
            "ee.xorq        q4, q4, q7          \n"
            /* ---- red ---- */
            "wsr.sar        %[s11]              \n"
            "ee.vsr.32      q5, q2              \n"
            "ee.vldbc.16    q6, %[m1f]          \n"
            "ee.andq        q5, q5, q6          \n" /* dstR5 */
            "wsr.sar        %[s8]               \n"
            "ee.vmul.s16    q5, q5, q4          \n" /* dstR5*inv>>8 */
            "wsr.sar        %[s3]               \n"
            "ee.vsr.32      q1, q1              \n"
            "ee.andq        q1, q1, q6          \n" /* srcR5 */
            "ee.vadds.s16   q1, q1, q5          \n" /* outR5 */
            /* ---- blue ---- */
            "ee.andq        q5, q2, q6          \n" /* dstB5 */
            "wsr.sar        %[s8]               \n"
            "ee.vmul.s16    q5, q5, q4          \n"
            "wsr.sar        %[s3]               \n"
            "ee.vsr.32      q0, q0              \n"
            "ee.andq        q0, q0, q6          \n" /* srcB5 */
            "ee.vadds.s16   q0, q0, q5          \n" /* outB5 */
            /* ---- green ---- */
            "wsr.sar        %[s5]               \n"
            "ee.vsr.32      q5, q2              \n"
            "ee.vldbc.16    q6, %[m3f]          \n"
            "ee.andq        q5, q5, q6          \n" /* dstG6 */
            "wsr.sar        %[s8]               \n"
            "ee.vmul.s16    q5, q5, q4          \n"
            "wsr.sar        %[s2]               \n"
            "ee.vsr.32      q3, q3              \n"
            "ee.andq        q3, q3, q6          \n" /* srcG6 */
            "ee.vadds.s16   q3, q3, q5          \n" /* outG6 */
            /* ---- repack (R<<11 | G<<5 | B) and store ---- */
            "wsr.sar        %[s11]              \n"
            "ee.vsl.32      q1, q1              \n"
            "wsr.sar        %[s5]               \n"
            "ee.vsl.32      q3, q3              \n"
            "ee.orq         q1, q1, q3          \n"
            "ee.orq         q1, q1, q0          \n"
            "ee.vst.128.ip  q1, %[dw], 16       \n"
            ".Lblend_end%=:                     \n"
            : [src] "+a"(src), [dr] "+a"(dr), [dw] "+a"(dw)
            : [n8] "a"(n8), [k255] "a"(&s_k255), [m1f] "a"(&s_m1f), [m3f] "a"(&s_m3f), [s2] "a"(2), [s3] "a"(3),
              [s5] "a"(5), [s8] "a"(8), [s11] "a"(11)
            : "memory");
        return;
    }

    __asm__ volatile(
        "ee.vldbc.16    q7, %[k255]         \n"
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
        "wsr.sar        %[s8]               \n"
        "ee.vmul.s16    q0, q0, q6          \n"
        "ee.vmul.s16    q1, q1, q6          \n"
        "ee.vmul.s16    q3, q3, q6          \n"
        "ee.vmul.s16    q4, q4, q6          \n"
        "ee.xorq        q4, q4, q7          \n"
        /* ---- red ---- */
        "wsr.sar        %[s11]              \n"
        "ee.vsr.32      q5, q2              \n"
        "ee.vldbc.16    q6, %[m1f]          \n"
        "ee.andq        q5, q5, q6          \n"
        "wsr.sar        %[s8]               \n"
        "ee.vmul.s16    q5, q5, q4          \n"
        "wsr.sar        %[s3]               \n"
        "ee.vsr.32      q1, q1              \n"
        "ee.andq        q1, q1, q6          \n"
        "ee.vadds.s16   q1, q1, q5          \n"
        /* ---- blue ---- */
        "ee.andq        q5, q2, q6          \n"
        "wsr.sar        %[s8]               \n"
        "ee.vmul.s16    q5, q5, q4          \n"
        "wsr.sar        %[s3]               \n"
        "ee.vsr.32      q0, q0              \n"
        "ee.andq        q0, q0, q6          \n"
        "ee.vadds.s16   q0, q0, q5          \n"
        /* ---- green ---- */
        "wsr.sar        %[s5]               \n"
        "ee.vsr.32      q5, q2              \n"
        "ee.vldbc.16    q6, %[m3f]          \n"
        "ee.andq        q5, q5, q6          \n"
        "wsr.sar        %[s8]               \n"
        "ee.vmul.s16    q5, q5, q4          \n"
        "wsr.sar        %[s2]               \n"
        "ee.vsr.32      q3, q3              \n"
        "ee.andq        q3, q3, q6          \n"
        "ee.vadds.s16   q3, q3, q5          \n"
        /* ---- repack + store ---- */
        "wsr.sar        %[s11]              \n"
        "ee.vsl.32      q1, q1              \n"
        "wsr.sar        %[s5]               \n"
        "ee.vsl.32      q3, q3              \n"
        "ee.orq         q1, q1, q3          \n"
        "ee.orq         q1, q1, q0          \n"
        "ee.vst.128.ip  q1, %[dw], 16       \n"
        ".Lblendga_end%=:                   \n"
        : [src] "+a"(src), [dr] "+a"(dr), [dw] "+a"(dw)
        : [n8] "a"(n8), [k255] "a"(&s_k255), [m1f] "a"(&s_m1f), [m3f] "a"(&s_m3f), [ga] "a"(&ga16), [s2] "a"(2),
          [s3] "a"(3), [s5] "a"(5), [s8] "a"(8), [s11] "a"(11)
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
    const uint16_t inv = (uint16_t)(255U - (sp >> 24));

    __asm__ volatile(
        /* persistent constants: q0=srcB5, q1=srcR5, q3=srcG6, q4=inv */
        "ee.vldbc.16    q0, %[b5]           \n"
        "ee.vldbc.16    q1, %[r5]           \n"
        "ee.vldbc.16    q3, %[g6]           \n"
        "ee.vldbc.16    q4, %[inv]          \n"
        "loopnez        %[n8], .Lfill_end%= \n"
        "ee.vld.128.ip  q2, %[dr], 16       \n"
        /* ---- red: q5 = (srcR5 + dstR5*inv>>8) << 11 ---- */
        "wsr.sar        %[s11]              \n"
        "ee.vsr.32      q5, q2              \n"
        "ee.vldbc.16    q6, %[m1f]          \n"
        "ee.andq        q5, q5, q6          \n"
        "wsr.sar        %[s8]               \n"
        "ee.vmul.s16    q5, q5, q4          \n"
        "ee.vadds.s16   q5, q5, q1          \n"
        "wsr.sar        %[s11]              \n"
        "ee.vsl.32      q5, q5              \n"
        /* ---- blue: q7 = srcB5 + dstB5*inv>>8 ---- */
        "ee.andq        q7, q2, q6          \n"
        "wsr.sar        %[s8]               \n"
        "ee.vmul.s16    q7, q7, q4          \n"
        "ee.vadds.s16   q7, q7, q0          \n"
        "ee.orq         q5, q5, q7          \n"
        /* ---- green: q7 = (srcG6 + dstG6*inv>>8) << 5 ---- */
        "wsr.sar        %[s5]               \n"
        "ee.vsr.32      q7, q2              \n"
        "ee.vldbc.16    q6, %[m3f]          \n"
        "ee.andq        q7, q7, q6          \n"
        "wsr.sar        %[s8]               \n"
        "ee.vmul.s16    q7, q7, q4          \n"
        "ee.vadds.s16   q7, q7, q3          \n"
        "wsr.sar        %[s5]               \n"
        "ee.vsl.32      q7, q7              \n"
        "ee.orq         q5, q5, q7          \n"
        "ee.vst.128.ip  q5, %[dw], 16       \n"
        ".Lfill_end%=:                      \n"
        : [dr] "+a"(dr), [dw] "+a"(dw)
        : [n8] "a"(n8), [b5] "a"(&srcB5), [r5] "a"(&srcR5), [g6] "a"(&srcG6), [inv] "a"(&inv), [m1f] "a"(&s_m1f),
          [m3f] "a"(&s_m3f), [s5] "a"(5), [s8] "a"(8), [s11] "a"(11)
        : "memory");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Self-test
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Scalar reference identical in rounding to the PIE routines (565-domain blend). */
static uint16_t ref_blend_px(uint16_t d, uint32_t sp, uint8_t ga)
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
    const uint32_t inv = 255U - a;
    const uint32_t dr5 = (d >> 11) & 31U;
    const uint32_t dg6 = (d >> 5) & 63U;
    const uint32_t db5 = d & 31U;
    const uint32_t or5 = (r >> 3) + ((dr5 * inv) >> 8);
    const uint32_t og6 = (g >> 2) + ((dg6 * inv) >> 8);
    const uint32_t ob5 = (b >> 3) + ((db5 * inv) >> 8);
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
            er_pie_blend_row_565(dst_pie, src, 64 / 8, ga);
            for (int i = 0; i < 64; i++)
            {
                dst_ref[i] = ref_blend_px(dst_ref[i], src[i], ga);
                if (dst_pie[i] != dst_ref[i])
                {
                    return false;
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
            dst_ref[i] = ref_blend_px(dst_ref[i], sp, 255U);
            if (dst_pie[i] != dst_ref[i])
            {
                return false;
            }
        }
    }
    return true;
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */
