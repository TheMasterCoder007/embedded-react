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

#include "native_renderer.h"
#include "vector.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FRAMEBUFFER_WIDTH 128
#define FRAMEBUFFER_HEIGHT 128

/*----------------------------------------------------------------------------------------------------------------------
 - Test harness (same shape as test_vector.c: capture blit calls into a flat ARGB framebuffer)
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    uint32_t* framebuffer;
    int framebuffer_width;
    int framebuffer_height;
} TestCtx;

static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    for (int row = y; row < y + height; row++)
        for (int column = x; column < x + width; column++)
            if (row >= 0 && column >= 0 && row < test_context->framebuffer_height
                && column < test_context->framebuffer_width)
                test_context->framebuffer[row * test_context->framebuffer_width + column] = argb;
}

static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)alpha;
    TestCtx* test_context = ctx;
    const uint32_t* source_pixels = source;
    const int pitch = (stride > 0) ? stride / (int)sizeof(uint32_t) : width;
    for (int row = 0; row < height; row++)
        for (int column = 0; column < width; column++)
        {
            const int framebuffer_y = y + row, framebuffer_x = x + column;
            if (framebuffer_y >= 0 && framebuffer_x >= 0 && framebuffer_y < test_context->framebuffer_height
                && framebuffer_x < test_context->framebuffer_width)
                test_context->framebuffer[framebuffer_y * test_context->framebuffer_width + framebuffer_x] =
                    source_pixels[row * pitch + column];
        }
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Test entry point
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Exercises the vector edge cache (ERUI_VECTOR_EDGE_CACHE) through er_vector_render_slot().
 *
 * Verifies that:
 *   - The FIRST render of a static tape does not record (two-touch promotion), the second records, and
 *     the third replays from the cache — with every render producing pixel-identical output, including
 *     the analytic-arc route (counted on replay too).
 *   - A replay under a partial clip paints exactly the reference pixels inside the clip and nothing
 *     outside — the moving-sibling damage-rect case the cache exists for.
 *   - er_vector_store() on the slot invalidates the entry AND disarms the promotion, so a tape that
 *     changes every render (an animated dial) never records or replays.
 *   - er_vector_free() invalidates, so a re-issued slot never replays the previous node's geometry.
 *   - A node whose geometry overflows the rasterizer edge pool falls back to uncached rendering with
 *     unchanged output (and stops re-trying, i.e. builds do not climb per render).
 *
 * With the cache compiled out the counters must stay 0 and er_vector_render_slot() must still paint
 * identically to er_vector_render(); the counter assertions are gated accordingly.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    static uint32_t framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
    static uint32_t reference[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
    TestCtx test_context = {framebuffer, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, &test_context};
    embedded_renderer_set_backend(&backend);

    er_vector_reset();
    er_vector_cache_stats_reset();
    er_vector_analytic_arc_count_reset();

    /* A mixed static tape: a filled+stroked polygon, a stroked open path with round joins, and a full
     * circle that routes through the analytic arc core. */
    const float tape[] = {/* shape 0: filled + stroked quad */
                          ER_VECTOR_OP_SHAPE,
                          0,
                          ER_VECTOR_OP_MOVE,
                          12,
                          12,
                          ER_VECTOR_OP_LINE,
                          52,
                          16,
                          ER_VECTOR_OP_LINE,
                          48,
                          52,
                          ER_VECTOR_OP_LINE,
                          14,
                          44,
                          ER_VECTOR_OP_CLOSE,
                          /* shape 1: open zigzag, stroked with round joins */
                          ER_VECTOR_OP_SHAPE,
                          1,
                          ER_VECTOR_OP_MOVE,
                          70,
                          20,
                          ER_VECTOR_OP_LINE,
                          90,
                          40,
                          ER_VECTOR_OP_LINE,
                          74,
                          58,
                          ER_VECTOR_OP_LINE,
                          96,
                          74,
                          /* shape 2: full circle -> analytic arc route (fill + stroke) */
                          ER_VECTOR_OP_SHAPE,
                          2,
                          ER_VECTOR_OP_ARC,
                          40,
                          90,
                          20,
                          0.0f,
                          6.2832f,
                          0};
    const int tape_count = (int)(sizeof(tape) / sizeof(tape[0]));
    const ERVectorPaint paints[3] = {
        {0xFF3355AAu, 0xFFFFFFFFu, 3.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0},
        {0x00000000u, 0xFF22CC44u, 5.0f, 0.0f, ER_VECTOR_CAP_ROUND, ER_VECTOR_JOIN_ROUND, ER_VECTOR_FILL_NONZERO, 0, 0},
        {0xFF884422u, 0xFFDDDDDDu, 4.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0},
    };

    const int slot = er_vector_store(-1, tape, tape_count, paints, 3, NULL, 0);
    if (slot < 0)
        return fail("setup: no storage slot");

    /* --- render #1: legacy path (two-touch: nothing recorded yet); this is the reference frame --- */
    memset(framebuffer, 0, sizeof(framebuffer));
    er_vector_render_slot(slot, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
    memcpy(reference, framebuffer, sizeof(framebuffer));
    const uint32_t arcs_per_render = er_vector_analytic_arc_count();
    if (arcs_per_render == 0)
        return fail("setup: circle did not take the analytic arc route");
#if ERUI_VECTOR_EDGE_CACHE
    if (er_vector_cache_builds() != 0 || er_vector_cache_hits() != 0)
        return fail("render #1 must neither record nor hit (two-touch)");
#endif

    /* --- render #2: records; output must not change --- */
    memset(framebuffer, 0, sizeof(framebuffer));
    er_vector_render_slot(slot, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
    if (memcmp(framebuffer, reference, sizeof(framebuffer)) != 0)
        return fail("recording render differs from the reference frame");
#if ERUI_VECTOR_EDGE_CACHE
    if (er_vector_cache_builds() != 1)
        return fail("render #2 did not record");
    if (er_vector_cache_hits() != 0)
        return fail("render #2 must not count as a hit");
#endif

    /* --- render #3: replays; output must not change, and the arc must still route analytically --- */
    memset(framebuffer, 0, sizeof(framebuffer));
    er_vector_render_slot(slot, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
    if (memcmp(framebuffer, reference, sizeof(framebuffer)) != 0)
        return fail("replayed render differs from the reference frame");
#if ERUI_VECTOR_EDGE_CACHE
    if (er_vector_cache_hits() != 1)
        return fail("render #3 did not replay from the cache");
    if (er_vector_analytic_arc_count() != 3 * arcs_per_render)
        return fail("replay did not route the circle through the analytic arc core");
#endif

    /* --- partial clip replay: exactly the reference pixels inside the clip, nothing outside --- */
    {
        const int clip_left = 30, clip_top = 24, clip_right = 84, clip_bottom = 70;
        memset(framebuffer, 0, sizeof(framebuffer));
        er_vector_render_slot(slot, 0, 0, clip_left, clip_top, clip_right, clip_bottom);
        for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++)
            for (int x = 0; x < FRAMEBUFFER_WIDTH; x++)
            {
                const int inside = (x >= clip_left && x < clip_right && y >= clip_top && y < clip_bottom);
                const uint32_t want = inside ? reference[y * FRAMEBUFFER_WIDTH + x] : 0u;
                if (framebuffer[y * FRAMEBUFFER_WIDTH + x] != want)
                    return fail(inside ? "partial-clip replay differs from the reference inside the clip"
                                       : "partial-clip replay painted outside the clip");
            }
    }

    /* --- er_vector_store() invalidates: new geometry must paint, and an every-render tape update
     *     (an animated node) must never record --- */
    {
        const float tape2[] = {ER_VECTOR_OP_SHAPE,
                               0,
                               ER_VECTOR_OP_MOVE,
                               100,
                               100,
                               ER_VECTOR_OP_LINE,
                               120,
                               100,
                               ER_VECTOR_OP_LINE,
                               120,
                               120,
                               ER_VECTOR_OP_LINE,
                               100,
                               120,
                               ER_VECTOR_OP_CLOSE};
        const ERVectorPaint red_paint = {0xFFFF0000u, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_NONZERO, 0, 0};
#if ERUI_VECTOR_EDGE_CACHE
        const uint32_t builds_before = er_vector_cache_builds();
        const uint32_t hits_before = er_vector_cache_hits();
#endif
        for (int render_index = 0; render_index < 4; render_index++)
        {
            /* Alternate the two tapes so every render sees a fresh store — the animated pattern. */
            const int use2 = render_index & 1;
            if (er_vector_store(slot,
                                use2 ? tape2 : tape,
                                use2 ? (int)(sizeof(tape2) / sizeof(tape2[0])) : tape_count,
                                use2 ? &red_paint : paints,
                                use2 ? 1 : 3,
                                NULL,
                                0)
                != slot)
                return fail("re-store changed the slot");
            memset(framebuffer, 0, sizeof(framebuffer));
            er_vector_render_slot(slot, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
            if (use2)
            {
                if (framebuffer[110 * FRAMEBUFFER_WIDTH + 110] != 0xFFFF0000u)
                    return fail("post-store render did not paint the new geometry");
                if (framebuffer[90 * FRAMEBUFFER_WIDTH + 40] != 0u)
                    return fail("post-store render painted the OLD geometry (stale cache)");
            }
            else if (memcmp(framebuffer, reference, sizeof(framebuffer)) != 0)
                return fail("post-store render of the original tape differs from the reference");
        }
#if ERUI_VECTOR_EDGE_CACHE
        if (er_vector_cache_builds() != builds_before)
            return fail("an every-render tape update recorded (two-touch failed to protect it)");
        if (er_vector_cache_hits() != hits_before)
            return fail("an every-render tape update replayed a stale entry");
#endif
    }

    /* --- er_vector_free(): a re-issued slot must not replay the previous node's geometry --- */
    {
        /* Re-establish a cached entry for the original tape first. */
        if (er_vector_store(slot, tape, tape_count, paints, 3, NULL, 0) != slot)
            return fail("re-store changed the slot");
        er_vector_render_slot(slot, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT); /* arm */
        er_vector_render_slot(slot, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT); /* record */
        er_vector_free(slot);
        const float tape3[] = {ER_VECTOR_OP_SHAPE,
                               0,
                               ER_VECTOR_OP_MOVE,
                               4,
                               4,
                               ER_VECTOR_OP_LINE,
                               24,
                               4,
                               ER_VECTOR_OP_LINE,
                               24,
                               24,
                               ER_VECTOR_OP_LINE,
                               4,
                               24,
                               ER_VECTOR_OP_CLOSE};
        const ERVectorPaint green_paint = {0xFF00FF00u, 0, 0.0f, 0.0f, 0, 0, ER_VECTOR_FILL_NONZERO, 0, 0};
        const int slot2 = er_vector_store(-1, tape3, (int)(sizeof(tape3) / sizeof(tape3[0])), &green_paint, 1, NULL, 0);
        if (slot2 != slot)
            return fail("freed slot was not re-issued (test setup assumption)");
        memset(framebuffer, 0, sizeof(framebuffer));
        er_vector_render_slot(slot2, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
        if (framebuffer[10 * FRAMEBUFFER_WIDTH + 10] != 0xFF00FF00u)
            return fail("re-issued slot did not paint its own geometry");
        if (framebuffer[90 * FRAMEBUFFER_WIDTH + 40] != 0u)
            return fail("re-issued slot replayed the freed node's geometry");
    }

    /* --- a moved node (new origin) misses, re-records at the new position, and paints correctly --- */
    {
        er_vector_free(slot);
        const int slot3 = er_vector_store(-1, tape, tape_count, paints, 3, NULL, 0);
        if (slot3 < 0)
            return fail("no slot for the move test");
        er_vector_render_slot(slot3, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT); /* arm */
        er_vector_render_slot(slot3, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT); /* record at (0,0) */
        memset(framebuffer, 0, sizeof(framebuffer));
        er_vector_render_slot(
            slot3, 6, 3, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT); /* moved: must NOT replay (0,0) edges */
        for (int y = 0; y < FRAMEBUFFER_HEIGHT - 3; y++)
            for (int x = 0; x < FRAMEBUFFER_WIDTH - 6; x++)
                if (framebuffer[(y + 3) * FRAMEBUFFER_WIDTH + (x + 6)] != reference[y * FRAMEBUFFER_WIDTH + x])
                    return fail("moved-origin render is not the reference shifted by the move");
        er_vector_free(slot3);
    }

    /* --- geometry too big to record: identical output every render, and no cache churn --- */
    {
        /* A sharply-reversing zigzag with round joins: every join fans many triangles, overflowing the
         * rasterizer edge pool — the recording must abort (and block), never truncating harder than
         * the legacy build does. */
        static float big[512];
        int big_count = 0;
        big[big_count++] = ER_VECTOR_OP_SHAPE;
        big[big_count++] = 0;
        big[big_count++] = ER_VECTOR_OP_MOVE;
        big[big_count++] = 4.0f;
        big[big_count++] = 60.0f;
        for (int segment_index = 0; segment_index < 160; segment_index++)
        {
            big[big_count++] = ER_VECTOR_OP_LINE;
            big[big_count++] = 5.0f + (float)(segment_index % 2) * 110.0f;
            big[big_count++] = 60.0f + (float)segment_index * 0.05f;
        }
        const ERVectorPaint big_paint = {0x00000000u,
                                         0xFF8080FFu,
                                         6.0f,
                                         0.0f,
                                         ER_VECTOR_CAP_ROUND,
                                         ER_VECTOR_JOIN_ROUND,
                                         ER_VECTOR_FILL_NONZERO,
                                         0,
                                         0};
        const int big_slot = er_vector_store(-1, big, big_count, &big_paint, 1, NULL, 0);
        if (big_slot < 0)
            return fail("no slot for the overflow test");
#if ERUI_VECTOR_EDGE_CACHE
        const uint32_t builds_before = er_vector_cache_builds();
#endif
        static uint32_t first[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
        memset(framebuffer, 0, sizeof(framebuffer));
        er_vector_render_slot(big_slot, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
        memcpy(first, framebuffer, sizeof(framebuffer));
        for (int frame_index = 0; frame_index < 3; frame_index++)
        {
            memset(framebuffer, 0, sizeof(framebuffer));
            er_vector_render_slot(big_slot, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
            if (memcmp(framebuffer, first, sizeof(framebuffer)) != 0)
                return fail("overflowing node renders inconsistently across frames");
        }
#if ERUI_VECTOR_EDGE_CACHE
        if (er_vector_cache_builds() != builds_before)
            return fail("overflowing node published a cache entry");
#endif
        er_vector_free(big_slot);
    }

    /* --- empty / invalid slots are safe no-ops --- */
    er_vector_render_slot(-1, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);
    er_vector_render_slot(1234, 0, 0, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT);

    er_vector_reset();
    printf("OK\n");
    return EXIT_SUCCESS;
}
