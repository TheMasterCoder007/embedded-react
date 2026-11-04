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
static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/**
 * @brief Premultiplied source-over of one ARGB8888 source pixel onto a destination pixel.
 *
 * The reference the scratch copy path must match: out.C = src.C + dst.C * (255 - src.A) / 255,
 * with src already premultiplied (as the engine's row buffers always are).
 */
static uint32_t over(uint32_t destination, uint32_t source)
{
    const uint8_t source_alpha = (uint8_t)((source >> 24) & 0xFFU);
    if (source_alpha == 0U)
        return destination;
    if (source_alpha == ER_ALPHA_OPAQUE)
        return source;
    const uint8_t inverse_alpha = (uint8_t)(ER_CHANNEL_MAX - source_alpha);
    const uint8_t out_alpha =
        (uint8_t)(source_alpha + (uint32_t)((destination >> 24) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
    const uint8_t out_red =
        (uint8_t)(((source >> 16) & 0xFFU) + (uint32_t)((destination >> 16) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
    const uint8_t out_green =
        (uint8_t)(((source >> 8) & 0xFFU) + (uint32_t)((destination >> 8) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
    const uint8_t out_blue =
        (uint8_t)((source & 0xFFU) + (uint32_t)(destination & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
    return ((uint32_t)out_alpha << 24) | ((uint32_t)out_red << 16) | ((uint32_t)out_green << 8) | out_blue;
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
        SCRATCH_WIDTH = 4,
        SCRATCH_HEIGHT = 4
    };
    uint32_t scratch[SCRATCH_WIDTH * SCRATCH_HEIGHT];

    /* Opaque blue background (premultiplied; opaque so straight == premultiplied). */
    const uint32_t background = 0xFF0000FFU;
    /* A premultiplied anti-aliased white edge pixel at ~25% coverage: A=64, and each color
     * channel already scaled by 64/255 = 64. This is the shape draw_glyph_aa emits at a soft edge. */
    const uint32_t edge = 0x40404040U;

    /* ---- Partial-alpha copy must blend over the existing background. ---- */
    for (int pixel_index = 0; pixel_index < SCRATCH_WIDTH * SCRATCH_HEIGHT; pixel_index++)
        scratch[pixel_index] = background;
    er_scratch_begin(scratch, SCRATCH_WIDTH, SCRATCH_HEIGHT, 0, 0);
    er_blit_copy(&edge, (int)sizeof(uint32_t), 0, 0, 1, 1);
    er_scratch_end();

    const uint32_t expected = over(background, edge);
    if (scratch[0] != expected)
    {
        fprintf(stderr,
                "got 0x%08X, expected blended 0x%08X (raw overwrite would be 0x%08X)\n",
                scratch[0],
                expected,
                edge);
        return fail("partial-alpha copy into scratch must source-over blend, not overwrite");
    }
    /* The blended result must still carry the blue it was laid over — the fringe bug zeroed it. */
    if ((scratch[0] & 0xFFU) <= (edge & 0xFFU))
        return fail("blended pixel lost the background blue underneath (the dark-fringe regression)");

    /* ---- Fully-opaque copy overwrites (unchanged fast-path behavior). ---- */
    for (int pixel_index = 0; pixel_index < SCRATCH_WIDTH * SCRATCH_HEIGHT; pixel_index++)
        scratch[pixel_index] = background;
    const uint32_t opaque = 0xFFFF0000U; /* red */
    er_scratch_begin(scratch, SCRATCH_WIDTH, SCRATCH_HEIGHT, 0, 0);
    er_blit_copy(&opaque, SCRATCH_WIDTH * (int)sizeof(uint32_t), 1, 1, 1, 1);
    er_scratch_end();
    if (scratch[1 * SCRATCH_WIDTH + 1] != opaque)
        return fail("fully-opaque copy into scratch must overwrite");

    /* ---- Fully-transparent copy leaves the destination untouched. ---- */
    for (int pixel_index = 0; pixel_index < SCRATCH_WIDTH * SCRATCH_HEIGHT; pixel_index++)
        scratch[pixel_index] = background;
    const uint32_t clear = 0x00000000U;
    er_scratch_begin(scratch, SCRATCH_WIDTH, SCRATCH_HEIGHT, 0, 0);
    er_blit_copy(&clear, SCRATCH_WIDTH * (int)sizeof(uint32_t), 2, 2, 1, 1);
    er_scratch_end();
    if (scratch[2 * SCRATCH_WIDTH + 2] != background)
        return fail("fully-transparent copy into scratch must leave the destination unchanged");

    printf("PASS: scratch copy blends partial alpha, overwrites opaque, skips transparent\n");
    return EXIT_SUCCESS;
}
