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
 * The software backend's fill, copy and blend callbacks against a per-pixel reference of the same
 * integer arithmetic, on random rects (clipped at every edge, every width up to a few vector strides)
 * and random premultiplied sources.
 *
 * CMake builds this file twice when the host can run AVX2: once as is, and once with -mavx2, where
 * the backend takes its eight-pixel rows. Both must match the reference bit for bit.
 */

/* The callbacks are static; the test drives them directly rather than through a scene. */
#include "renderer_backend.c"

#include <stdio.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 61
#define FB_H 23
#define SRC_W 48
#define SRC_H 30
#define OPS 300

/*----------------------------------------------------------------------------------------------------------------------
 - Reference
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_ref[FB_W * FB_H];
static uint32_t s_src[SRC_W * SRC_H];
static uint32_t s_rng = 0x2545F491U;

static uint32_t rnd(uint32_t n)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng % n;
}

static uint32_t ref_div255(uint32_t x)
{
    return (x + 128U + ((x + 128U) >> 8)) >> 8;
}

/** @brief Premultiplied source-over onto an opaque pixel; the result is opaque. */
static uint32_t ref_over(uint32_t d, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb)
{
    const uint32_t inv = 255U - sa;
    const uint32_t r = sr + ref_div255(((d >> 16) & 0xFFU) * inv);
    const uint32_t g = sg + ref_div255(((d >> 8) & 0xFFU) * inv);
    const uint32_t b = sb + ref_div255((d & 0xFFU) * inv);
    return 0xFF000000U | (r << 16) | (g << 8) | b;
}

/** @brief The reference pixel under (x + col, y + row), or NULL when that lands off the framebuffer. */
static uint32_t* ref_px(int x, int y, int col, int row)
{
    const int fx = x + col, fy = y + row;
    return (fx < 0 || fy < 0 || fx >= FB_W || fy >= FB_H) ? NULL : &s_ref[fy * FB_W + fx];
}

static void ref_fill(uint32_t argb, int x, int y, int w, int h)
{
    const uint32_t a = argb >> 24;
    const uint32_t sr = ref_div255(((argb >> 16) & 0xFFU) * a);
    const uint32_t sg = ref_div255(((argb >> 8) & 0xFFU) * a);
    const uint32_t sb = ref_div255((argb & 0xFFU) * a);
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
        {
            uint32_t* d = ref_px(x, y, col, row);
            if (d && a == 255U)
                *d = argb;
            else if (d && a != 0U)
                *d = ref_over(*d, a, sr, sg, sb);
        }
}

/** @brief copy_rect at ga 255 (opaque replaces, clear keeps), blend_rect below it (every channel scaled). */
static void ref_blend(uint32_t ga, int x, int y, int w, int h)
{
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
        {
            uint32_t* d = ref_px(x, y, col, row);
            const uint32_t sp = s_src[row * SRC_W + col];
            const uint32_t sa = ref_div255((sp >> 24) * ga);
            if (!d || sa == 0U)
                continue;
            if (sa == 255U)
                *d = sp;
            else
                *d = ref_over(*d,
                              sa,
                              ref_div255(((sp >> 16) & 0xFFU) * ga),
                              ref_div255(((sp >> 8) & 0xFFU) * ga),
                              ref_div255((sp & 0xFFU) * ga));
        }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A premultiplied pixel: mostly opaque or clear runs, the rest any alpha. */
static uint32_t rnd_premul(void)
{
    const uint32_t kind = rnd(4);
    const uint32_t a = kind == 0 ? 0U : (kind == 1 ? 255U : rnd(256));
    const uint32_t r = rnd(a + 1U), g = rnd(a + 1U), b = rnd(a + 1U);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void fill_src(void)
{
    /* Whole runs of one alpha class too, so the all-opaque and all-clear eight-pixel shortcuts are taken. */
    const uint32_t mode = rnd(3);
    for (int i = 0; i < SRC_W * SRC_H; i++)
    {
        if (mode == 0)
            s_src[i] = rnd_premul();
        else if (mode == 1)
            s_src[i] = 0xFF000000U | rnd(0x1000000U);
        else
            s_src[i] = rnd(2) ? 0U : rnd_premul();
    }
}

static void rnd_rect(int* x, int* y, int* w, int* h)
{
    *w = 1 + (int)rnd(SRC_W);
    *h = 1 + (int)rnd(SRC_H);
    *x = (int)rnd(FB_W + 20) - 10 - *w / 2;
    *y = (int)rnd(FB_H + 10) - 5 - *h / 2;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Main
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    if (!er_software_backend_init(FB_W, FB_H))
    {
        fprintf(stderr, "FAIL: backend init\n");
        return EXIT_FAILURE;
    }
    uint32_t* fb = er_software_framebuffer();
    for (int i = 0; i < FB_W * FB_H; i++)
        fb[i] = s_ref[i] = 0xFF000000U | rnd(0x1000000U);

    static const char* const names[] = {"fill", "copy", "blend"};
    for (int op = 0; op < 3 * OPS; op++)
    {
        int x, y, w, h;
        rnd_rect(&x, &y, &w, &h);
        const int kind = op % 3;
        if (kind == 0)
        {
            const uint32_t a = rnd(3) == 0 ? 255U : rnd(256);
            const uint32_t argb = (a << 24) | rnd(0x1000000U);
            fill_rect_cb(argb, x, y, w, h, &s_ctx);
            ref_fill(argb, x, y, w, h);
        }
        else
        {
            fill_src();
            const uint32_t ga = kind == 1 ? 255U : (rnd(4) == 0 ? 255U : 1U + rnd(254));
            if (kind == 1)
                copy_rect_cb(s_src, SRC_W * 4, x, y, w, h, &s_ctx);
            else
                blend_rect_cb(s_src, SRC_W * 4, (uint8_t)ga, x, y, w, h, &s_ctx);
            ref_blend(ga, x, y, w, h);
        }
        if (memcmp(fb, s_ref, sizeof(s_ref)) != 0)
        {
            for (int i = 0; i < FB_W * FB_H; i++)
                if (fb[i] != s_ref[i])
                {
                    fprintf(stderr,
                            "FAIL: %s #%d (%d,%d %dx%d): pixel (%d,%d) is %08X, expected %08X\n",
                            names[kind],
                            op / 3,
                            x,
                            y,
                            w,
                            h,
                            i % FB_W,
                            i / FB_W,
                            (unsigned)fb[i],
                            (unsigned)s_ref[i]);
                    break;
                }
            er_software_backend_destroy();
            return EXIT_FAILURE;
        }
    }

#if defined(__AVX2__)
    printf("OK (AVX2 rows)\n");
#else
    printf("OK\n");
#endif
    er_software_backend_destroy();
    return EXIT_SUCCESS;
}
