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
 * Regression: er_blit_copy into an active scratch buffer must SOURCE-OVER blend, exactly like
 * every backend's copy_rect blends onto the framebuffer — not raw-overwrite the destination.
 *
 * The raw-overwrite bug dropped the background already painted under anti-aliased glyph edges
 * (which carry partial alpha), so text composited through an opacity group or a transform source
 * gained a dark fringe once the scratch was blended out. This test exercises the scratch copy path
 * directly with a known partial-alpha pixel — no font rendering needed — so it stays deterministic.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/**
 * @brief Premultiplied source-over of one ARGB8888 source pixel onto a destination pixel.
 *
 * The reference the scratch copy path must match: out.C = src.C + dst.C * (255 - src.A) / 255,
 * with src already premultiplied (as the engine's row buffers always are).
 */
static uint32_t over(uint32_t d, uint32_t sp)
{
    const uint8_t sa = (uint8_t)((sp >> 24) & 0xFFU);
    if (sa == 0U)
        return d;
    if (sa == 255U)
        return sp;
    const uint8_t inv = (uint8_t)(255U - sa);
    const uint8_t oa = (uint8_t)(sa + (uint32_t)((d >> 24) & 0xFFU) * inv / 255U);
    const uint8_t or_ = (uint8_t)(((sp >> 16) & 0xFFU) + (uint32_t)((d >> 16) & 0xFFU) * inv / 255U);
    const uint8_t og = (uint8_t)(((sp >> 8) & 0xFFU) + (uint32_t)((d >> 8) & 0xFFU) * inv / 255U);
    const uint8_t ob = (uint8_t)((sp & 0xFFU) + (uint32_t)(d & 0xFFU) * inv / 255U);
    return ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies er_blit_copy blends (not overwrites) partial-alpha pixels into scratch.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    /* No backend is needed: with a scratch buffer active the blit writes into it directly and never
     * forwards to the backend. Reset state so the clip stack starts empty. */
    embedded_renderer_set_backend(NULL);

    enum
    {
        W = 4,
        H = 4
    };
    uint32_t scratch[W * H];

    /* Opaque blue background (premultiplied; opaque so straight == premultiplied). */
    const uint32_t bg = 0xFF0000FFU;
    /* A premultiplied anti-aliased white edge pixel at ~25% coverage: A=64, and each color
     * channel already scaled by 64/255 = 64. This is the shape draw_glyph_aa emits at a soft edge. */
    const uint32_t edge = 0x40404040U;

    /* ---- Partial-alpha copy must blend over the existing background. ---- */
    for (int i = 0; i < W * H; i++)
        scratch[i] = bg;
    er_scratch_begin(scratch, W, H, 0, 0);
    er_blit_copy(&edge, W * (int)sizeof(uint32_t), 0, 0, 1, 1);
    er_scratch_end();

    const uint32_t expect = over(bg, edge);
    if (scratch[0] != expect)
    {
        fprintf(stderr, "got 0x%08X, expected blended 0x%08X (raw overwrite would be 0x%08X)\n",
                scratch[0], expect, edge);
        return fail("partial-alpha copy into scratch must source-over blend, not overwrite");
    }
    /* The blended result must still carry the blue it was laid over — the fringe bug zeroed it. */
    if ((scratch[0] & 0xFFU) <= (edge & 0xFFU))
        return fail("blended pixel lost the background blue underneath (the dark-fringe regression)");

    /* ---- Fully-opaque copy overwrites (unchanged fast-path behavior). ---- */
    for (int i = 0; i < W * H; i++)
        scratch[i] = bg;
    const uint32_t opaque = 0xFFFF0000U; /* red */
    er_scratch_begin(scratch, W, H, 0, 0);
    er_blit_copy(&opaque, W * (int)sizeof(uint32_t), 1, 1, 1, 1);
    er_scratch_end();
    if (scratch[1 * W + 1] != opaque)
        return fail("fully-opaque copy into scratch must overwrite");

    /* ---- Fully-transparent copy leaves the destination untouched. ---- */
    for (int i = 0; i < W * H; i++)
        scratch[i] = bg;
    const uint32_t clear = 0x00000000U;
    er_scratch_begin(scratch, W, H, 0, 0);
    er_blit_copy(&clear, W * (int)sizeof(uint32_t), 2, 2, 1, 1);
    er_scratch_end();
    if (scratch[2 * W + 2] != bg)
        return fail("fully-transparent copy into scratch must leave the destination unchanged");

    printf("PASS: scratch copy blends partial alpha, overwrites opaque, skips transparent\n");
    return EXIT_SUCCESS;
}
